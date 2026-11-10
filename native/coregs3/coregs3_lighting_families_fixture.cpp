// CTest native_coregs3_lighting_families: the body program and the seven measured per-item VU1 lighting families of coregs3_lighting.{hpp,cpp}
// on hand-authored data only. No game data is read.
//
// The expected colours are the outputs of the project's own AOT fragments of the closed VU1 image (clean room: the
// emitter output, driven through the round 14 probe harness), not values re-derived from the module under test. They
// pin, per program:
//   - the first-power nine-qword formula (ITOP 0x0B08): L_j = max(dot(M_j,N), 0) and C3 + C4*L0 + ... + C7*L3;
//   - the power-8 nine-qword formula (ITOP 0x0250): lanes 2/3 squared three times;
//   - the power-16 nine-qword formulas (ITOP 0x03D8 and the direct 0x0418): lanes 2/3 squared four times;
//   - the three-qword gain/clamp/bias formula (ITOP 0x14D8), lane 3 included;
//   - the six-qword no-colour Phong-style formula (ITOP 0x0C68): the signed t, the single max after the weighted sum,
//     the eighth power of the summed x component and the constant alpha 128;
//   - the no-block pass-through (ITOP 0x0080), alpha included;
//   - the per-item selection and the fallback statuses (invalid/unmeasured selector, missing/uncertain/variant/non-finite
//     block, missing channels, invalid math), never a guessed colour.
//
// The probe cases that back the expected values are recorded in docs/COREGS3-HOOKS.md section 14.
#include "coregs3_lighting.hpp"

#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

std::uint32_t itop_word(std::uint32_t dispatch) { return 0x04000000u | (dispatch / 8u); }

using Qword = std::array<float, 4>;
using Qwords = std::array<Qword, 9>;

ItemLightState make_state(LightProgram program, const Qwords &block) {
  ItemLightState state;
  state.program = program;
  for (std::uint32_t qword = 0; qword < 9; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane) state.qword[qword][lane] = block[qword][lane];
  return state;
}

// The nine-qword block used by the first/power cases: three identity rows, a constant, four slopes and the clamp.
Qwords nine_block(float first = 1.0f, float second = 1.0f, float third = 1.0f) {
  Qwords block{};
  block[0] = {first, 0.0f, 0.0f, 0.0f};
  block[1] = {0.0f, second, 0.0f, 0.0f};
  block[2] = {0.0f, 0.0f, third, 0.0f};
  return block;
}

Qwords identity_nine() {
  Qwords block{};
  block[0] = {1.0f, 0.0f, 0.0f, 0.0f};
  block[1] = {0.0f, 1.0f, 0.0f, 0.0f};
  block[2] = {0.0f, 0.0f, 1.0f, 0.0f};
  block[3] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (std::uint32_t qword = 4; qword < 8; ++qword) block[qword] = {10.0f, 10.0f, 10.0f, 10.0f};
  block[8] = {255.0f, 255.0f, 255.0f, 128.0f};
  return block;
}

MeshVertex normal_vertex(std::int8_t x, std::int8_t y, std::int8_t z) {
  MeshVertex vertex;
  vertex.normal80 = {x, y, z};
  vertex.has_normal80 = true;
  return vertex;
}

MeshVertex float_normal_vertex(float x, float y, float z) {
  MeshVertex vertex;
  vertex.normal80_float = {x, y, z};
  vertex.has_normal80_float = true;
  return vertex;
}

std::array<std::uint8_t, 4> shade(const ItemLightState &state, const MeshVertex &vertex) {
  std::array<std::uint8_t, 4> rgba = {1, 2, 3, 4};
  require(shade_item_vertex(state, vertex, rgba) == LightStatus::ok, "the hand case must shade");
  return rgba;
}

std::array<std::uint8_t, 4> colour(std::uint32_t red, std::uint32_t green, std::uint32_t blue, std::uint32_t alpha) {
  return {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green), static_cast<std::uint8_t>(blue),
          static_cast<std::uint8_t>(alpha)};
}

// ---- program selection ---------------------------------------------------------------------------------------------

void check_program_selection() {
  require(light_program_from_dispatch(0x1058) == LightProgram::body_paint, "0x1058 is body_paint");
  require(light_program_from_dispatch(0x0B08) == LightProgram::nine_qword_first, "0x0B08 is nine_qword_first");
  require(light_program_from_dispatch(0x0250) == LightProgram::nine_qword_pow8, "0x0250 is nine_qword_pow8");
  require(light_program_from_dispatch(0x03D8) == LightProgram::nine_qword_pow16, "0x03D8 is nine_qword_pow16");
  require(light_program_from_dispatch(0x0418) == LightProgram::nine_qword_direct, "0x0418 is nine_qword_direct");
  require(light_program_from_dispatch(0x0C68) == LightProgram::six_qword_no_colour, "0x0C68 is six_qword_no_colour");
  require(light_program_from_dispatch(0x14D8) == LightProgram::three_qword_gain, "0x14D8 is three_qword_gain");
  require(light_program_from_dispatch(0x1508) == LightProgram::three_qword_gain,
          "0x1508 (the sky) is the same three-qword gain formula");
  require(light_program_from_dispatch(0x0080) == LightProgram::passthrough, "0x0080 is passthrough");
  require(light_program_from_dispatch(0x00A0) == LightProgram::morph_passthrough,
          "0x00A0 is the two-key morph pre-pass");
  require(light_program_from_dispatch(0x1278) == LightProgram::body_prepass,
          "0x1278 is the body/paint pre-pass");
  require(light_program_from_dispatch(0x1234) == LightProgram::none, "an unmeasured dispatch is none");

  CapturedItem item;
  item.run_valid = 1;
  item.run_itop = itop_word(0x0B08);
  require(item_dispatch(item) == 0x0B08 && light_program_of_item(item) == LightProgram::nine_qword_first,
          "the item dispatch masks the ITOP immediate and scales by eight");
  item.run_valid = 0;
  require(item_dispatch(item) == 0 && light_program_of_item(item) == LightProgram::none,
          "an invalid run pair has no program");
}

// ---- decode_item_light ---------------------------------------------------------------------------------------------

CapturedItem block_item(LightProgram program, std::uint32_t qwords, std::uint32_t command, std::uint32_t source) {
  CapturedItem item;
  item.run_valid = 1;
  switch (program) {
  case LightProgram::body_paint: item.run_itop = itop_word(0x1058); break;
  case LightProgram::nine_qword_first: item.run_itop = itop_word(0x0B08); break;
  case LightProgram::nine_qword_pow8: item.run_itop = itop_word(0x0250); break;
  case LightProgram::nine_qword_pow16: item.run_itop = itop_word(0x03D8); break;
  case LightProgram::nine_qword_direct: item.run_itop = itop_word(0x0418); break;
  case LightProgram::six_qword_no_colour: item.run_itop = itop_word(0x0C68); break;
  case LightProgram::three_qword_gain: item.run_itop = itop_word(0x14D8); break;
  case LightProgram::passthrough: item.run_itop = itop_word(0x0080); break;
  case LightProgram::morph_passthrough: item.run_itop = itop_word(0x00A0); break;
  case LightProgram::body_prepass: item.run_itop = itop_word(0x1278); break;
  default: break;
  }
  item.light_block_qwords = qwords;
  item.light_block_command = command;
  item.light_block_source = 0x00130000u;
  item.light_source = source;
  for (std::uint32_t qword = 0; qword < qwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      item.light_block[qword][lane] = float_bits(static_cast<float>(qword * 4 + lane + 1));
  return item;
}

void check_decode_statuses() {
  ItemLightState state;
  CapturedItem item = block_item(LightProgram::nine_qword_first, 9, kNineQwordLightBlockCommand, kLightItemUploaded);
  require(decode_item_light(item, state) == LightStatus::ok && state.program == LightProgram::nine_qword_first,
          "a measured uploaded nine-qword block decodes");

  // An uploaded block must carry its own tag word.
  item.light_block_command = kBodyPaintLightBlockCommand;
  require(decode_item_light(item, state) == LightStatus::block_variant, "an uploaded wrong command is a variant");
  item.light_block_command = kNineQwordLightBlockCommand;
  item.light_block_qwords = 6;
  require(decode_item_light(item, state) == LightStatus::block_variant, "a wrong qword count is a variant");

  // A carried block has no tag of its own: its qword count is the variant and a known command must match, but zero is
  // accepted.
  item = block_item(LightProgram::nine_qword_first, 9, 0, kLightItemCarried);
  require(decode_item_light(item, state) == LightStatus::ok, "a carried block with no command word is accepted");
  item.light_block_command = kBodyPaintLightBlockCommand;
  require(decode_item_light(item, state) == LightStatus::block_variant, "a known carried command must match");
  item = block_item(LightProgram::nine_qword_first, 9, kNineQwordLightBlockCommand, kLightItemCarriedUncertain);
  require(decode_item_light(item, state) == LightStatus::uncertain_block, "an uncertain carried block falls back");
  item = block_item(LightProgram::nine_qword_first, 9, kNineQwordLightBlockCommand, kLightItemNone);
  require(decode_item_light(item, state) == LightStatus::no_block, "a missing block falls back");

  item = block_item(LightProgram::nine_qword_first, 9, kNineQwordLightBlockCommand, kLightItemUploaded);
  item.light_block[3][1] = 0x7FC00000u;  // NaN
  require(decode_item_light(item, state) == LightStatus::block_not_finite, "a NaN qword falls back");

  item = block_item(LightProgram::nine_qword_first, 9, kNineQwordLightBlockCommand, kLightItemUploaded);
  item.run_valid = 0;
  require(decode_item_light(item, state) == LightStatus::no_program, "an invalid run pair falls back");
  item.run_valid = 1;
  item.run_itop = itop_word(0x1234);
  require(decode_item_light(item, state) == LightStatus::no_program, "an unmeasured selector falls back");

  // The three-qword and body forms have their own qword counts and commands.
  item = block_item(LightProgram::three_qword_gain, 3, kThreeQwordLightBlockCommand, kLightItemUploaded);
  require(decode_item_light(item, state) == LightStatus::ok, "the three-qword form decodes");
  item = block_item(LightProgram::six_qword_no_colour, 6, kBodyPaintLightBlockCommand, kLightItemUploaded);
  require(decode_item_light(item, state) == LightStatus::ok, "the six-qword no-colour form decodes");

  // The passthrough program reads no block, so no light_source can gate it.
  item = block_item(LightProgram::passthrough, 0, 0, kLightItemNone);
  require(decode_item_light(item, state) == LightStatus::ok, "passthrough ignores a missing block");
  item = block_item(LightProgram::passthrough, 9, 0, kLightItemCarriedUncertain);
  require(decode_item_light(item, state) == LightStatus::ok, "passthrough ignores an uncertain block");

  // TASK-090 world families: the morph program reads no block; the body pre-pass reads the six-qword body form.
  item = block_item(LightProgram::morph_passthrough, 0, 0, kLightItemNone);
  require(decode_item_light(item, state) == LightStatus::ok && state.program == LightProgram::morph_passthrough,
          "the morph program ignores a missing block");
  item = block_item(LightProgram::morph_passthrough, 6, kBodyPaintLightBlockCommand, kLightItemCarriedUncertain);
  require(decode_item_light(item, state) == LightStatus::ok, "the morph program ignores an uncertain block");
  item = block_item(LightProgram::body_prepass, 6, kBodyPaintLightBlockCommand, kLightItemUploaded);
  require(decode_item_light(item, state) == LightStatus::ok && state.program == LightProgram::body_prepass,
          "the body pre-pass reads the six-qword body block");
  item = block_item(LightProgram::body_prepass, 9, kNineQwordLightBlockCommand, kLightItemUploaded);
  require(decode_item_light(item, state) == LightStatus::block_variant,
          "the body pre-pass refuses the nine-qword form");
}

// ---- the formulas, from the AOT fragments --------------------------------------------------------------------------

void check_nine_first() {
  // 0x0B48, N = (127,0,0): L0 = 1 and every colour lane adds q4 to q3.
  Qwords block = nine_block();
  block[3] = {10.0f, 20.0f, 30.0f, 40.0f};
  block[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  block[5] = {5.0f, 6.0f, 7.0f, 8.0f};
  block[6] = {9.0f, 10.0f, 11.0f, 12.0f};
  block[7] = {13.0f, 14.0f, 15.0f, 16.0f};
  block[8] = {255.0f, 255.0f, 255.0f, 128.0f};
  const ItemLightState state = make_state(LightProgram::nine_qword_first, block);
  require(shade(state, normal_vertex(127, 0, 0)) == colour(11, 22, 33, 44), "first-power axis case");

  // A slanted normal against distinct matrix rows; per-lane constants and alpha lane 3.
  block = {};
  block[0] = {0.5f, -0.25f, 0.0f, 0.0f};
  block[1] = {0.0f, 0.75f, -0.5f, 0.0f};
  block[2] = {-0.25f, 0.0f, 1.0f, 0.0f};
  block[3] = {5.0f, 6.0f, 7.0f, 8.0f};
  block[4] = {0.1f, 0.2f, 0.3f, 0.05f};
  block[5] = {0.2f, 0.3f, 0.4f, 0.06f};
  block[6] = {0.3f, 0.4f, 0.5f, 0.07f};
  block[7] = {0.4f, 0.5f, 0.6f, 0.08f};
  block[8] = {255.0f, 255.0f, 255.0f, 128.0f};
  require(shade(make_state(LightProgram::nine_qword_first, block), normal_vertex(73, -51, 89)) ==
              colour(5, 6, 7, 8),
          "first-power slanted case");

  // A negative direction is clamped before the coefficient sum: the output is exactly C3.
  block = {};
  block[0] = {0.0f, 1.0f, 0.0f, 0.0f};
  block[1] = {0.0f, 0.0f, 1.0f, 0.0f};
  block[2] = {1.0f, 0.0f, 0.0f, 0.0f};
  block[3] = {10.0f, 20.0f, 30.0f, 40.0f};
  block[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  block[5] = {5.0f, 6.0f, 7.0f, 8.0f};
  block[6] = {9.0f, 10.0f, 11.0f, 12.0f};
  block[7] = {13.0f, 14.0f, 15.0f, 16.0f};
  block[8] = {255.0f, 255.0f, 255.0f, 128.0f};
  require(shade(make_state(LightProgram::nine_qword_first, block), normal_vertex(0, -127, 0)) ==
              colour(10, 20, 30, 40),
          "a negative direction contributes exactly zero");
}

void check_nine_powers() {
  const Qwords block = identity_nine();
  // N = (127,127,115)/127: L = (1, 1, 0.9055, 0). The three variants differ only in the L2/L3 powers.
  const MeshVertex vertex = normal_vertex(127, 127, 115);
  require(shade(make_state(LightProgram::nine_qword_first, block), vertex) == colour(29, 29, 29, 29),
          "first power sums L2 once");
  require(shade(make_state(LightProgram::nine_qword_pow8, block), vertex) == colour(24, 24, 24, 24),
          "power-8 raises L2/L3 with three squarings");
  require(shade(make_state(LightProgram::nine_qword_pow16, block), vertex) == colour(22, 22, 22, 22),
          "power-16 raises L2/L3 with four squarings");
  // N = (127,127,126): L2 = 0.99213; power-8 gives 29 and power-16 gives 28.
  const MeshVertex near = normal_vertex(127, 127, 126);
  require(shade(make_state(LightProgram::nine_qword_pow8, block), near) == colour(29, 29, 29, 29),
          "power-8 second case");
  require(shade(make_state(LightProgram::nine_qword_pow16, block), near) == colour(28, 28, 28, 28),
          "power-16 second case");
  // The direct 0x0418 program reads V3-32 unit float normals and has no prologue.
  const ItemLightState direct = make_state(LightProgram::nine_qword_direct, block);
  require(shade(direct, float_normal_vertex(1.0f, 0.5f, 0.25f)) == colour(15, 15, 15, 15),
          "the direct program takes a float normal");
  MeshVertex no_float = normal_vertex(127, 0, 0);
  std::array<std::uint8_t, 4> untouched = colour(1, 2, 3, 4);
  require(shade_item_vertex(direct, no_float, untouched) == LightStatus::missing_channel,
          "the direct program refuses a V3-8 normal");
}

void check_three_qword() {
  ItemLightState state;
  state.program = LightProgram::three_qword_gain;
  state.qword[0] = {2.0f, 0.5f, 1.0f, 0.0f};     // gain
  state.qword[1] = {255.0f, 200.0f, 100.0f, 128.0f};  // clamp
  state.qword[2] = {10.0f, 20.0f, 30.0f, 40.0f};  // bias
  MeshVertex vertex;
  vertex.color = colour(64, 128, 32, 255);
  vertex.has_color = true;
  require(shade(state, vertex) == colour(138, 84, 62, 40), "three-qword gain/clamp/bias");

  state.qword[0] = {-1.0f, 1.0f, 0.0f, 1.0f};
  state.qword[1] = {255.0f, 255.0f, 255.0f, 255.0f};
  require(shade(state, vertex) == colour(0, 148, 30, 255), "three-qword clamps the low side and lane 3");
}

void check_six_qword() {
  ItemLightState state;
  state.program = LightProgram::six_qword_no_colour;
  state.qword[0] = {0.0f, 0.0f, 0.0f, 0.0f};
  state.qword[1] = {0.0f, 0.0f, 0.0f, 0.0f};
  state.qword[2] = {1.0f, 2.0f, 3.0f, 0.0f};
  state.qword[3] = {0.0f, 0.0f, 0.0f, 0.0f};   // B
  state.qword[4] = {10.0f, 10.0f, 10.0f, 10.0f};  // the 0xCD scale
  MeshVertex vertex;
  vertex.normal80 = {127, 0, 0};
  vertex.has_normal80 = true;
  vertex.raw_position = {0, 0, 10};
  vertex.has_raw_position = true;
  require(shade(state, vertex) == colour(9, 9, 9, 128), "six-qword specular, alpha 128");

  // A negative t0 is not clamped before the weighted sum: the fragment's L.x is lower by q0.x * t0.
  state.qword[0] = {0.5f, 0.5f, 0.5f, 0.0f};
  state.qword[1] = {1.0f, 1.0f, 1.0f, 0.0f};
  state.qword[2] = {2.0f, 2.0f, 2.0f, 0.0f};
  state.qword[3] = {10.0f, 20.0f, 30.0f, 0.0f};
  state.qword[4] = {3.0f, 3.0f, 3.0f, 3.0f};
  vertex.normal80 = {73, -51, 89};
  vertex.raw_position = {100, 200, 300};
  require(shade(state, vertex) == colour(2, 2, 2, 128), "six-qword signed t and scalar L.x");

  // The output uses lane 0 of the clamped L only: L1/L2 alone give zero.
  state = ItemLightState{};
  state.program = LightProgram::six_qword_no_colour;
  state.qword[2] = {0.0f, 2.0f, 0.0f, 0.0f};  // 0xCB pushes L.x with t2
  state.qword[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  vertex.normal80 = {127, 0, 0};
  vertex.raw_position = {0, 0, 10};
  require(shade(state, vertex) == colour(0, 0, 0, 128), "six-qword uses L.x only");
}

void check_world_family_formulas() {
  // The sky's 0x1508 entry is the same three-qword gain: an identity block emits the colour with alpha clamped to 128.
  ItemLightState state;
  state.program = LightProgram::three_qword_gain;
  state.qword[0] = {1.0f, 1.0f, 1.0f, 1.0f};
  state.qword[1] = {255.0f, 255.0f, 255.0f, 128.0f};
  state.qword[2] = {0.0f, 0.0f, 0.0f, 0.0f};
  MeshVertex vertex;
  vertex.color = colour(200, 100, 50, 255);
  vertex.has_color = true;
  require(shade(state, vertex) == colour(200, 100, 50, 128), "the sky three-qword gain clamps alpha to 128");

  // The 0x1278 body pre-pass uses the full MODE 2 lane as the body position. The block below makes the factor 0.5:
  // N = +x, D = (100,200,300) - (0,0,0), so the cosine is positive and t is 0; output = base * C3 = 50 * 0.5 = 25.
  ItemLightState body;
  body.program = LightProgram::body_prepass;
  body.qword[0] = {1.0f, 0.0f, 0.0f, 0.0f};
  body.qword[1] = {0.0f, 1.0f, 0.0f, 0.0f};
  body.qword[2] = {0.0f, 0.0f, 1.0f, 0.0f};
  body.qword[3] = {0.0f, 0.0f, 0.0f, 0.0f};
  body.qword[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  body.qword[5] = {0.0f, 0.0f, 0.0f, 0.0f};
  MeshVertex lane;
  lane.normal = {127, 0, 0};
  lane.has_normal = true;
  lane.lane_position = {100, 200, 300};
  lane.has_lane_position = true;
  lane.color = colour(50, 60, 70, 128);
  lane.has_color = true;
  require(shade(body, lane) == colour(25, 30, 35, 64), "the body pre-pass shades the full int32 lane position");
  MeshVertex raw_only = lane;
  raw_only.has_lane_position = false;
  raw_only.raw_position = {100, 200, 300};
  raw_only.has_raw_position = true;
  require(shade(body, raw_only) == colour(25, 30, 35, 64), "the same position in the raw halfword gives the same colour");
  MeshVertex no_position = lane;
  no_position.has_lane_position = false;
  std::array<std::uint8_t, 4> untouched = colour(1, 2, 3, 4);
  require(shade_item_vertex(body, no_position, untouched) == LightStatus::missing_channel,
          "the body pre-pass refuses a vertex without any position");
}

void check_passthrough() {
  ItemLightState state;
  state.program = LightProgram::passthrough;
  MeshVertex vertex;
  vertex.color = colour(64, 128, 32, 255);
  vertex.has_color = true;
  require(shade(state, vertex) == colour(64, 128, 32, 255), "passthrough keeps RGB and alpha");
  vertex.color = colour(1, 2, 3, 4);
  require(shade(state, vertex) == colour(1, 2, 3, 4), "passthrough keeps a small alpha");
}

// ---- per-vertex and per-item fallbacks -----------------------------------------------------------------------------

void check_vertex_fallbacks() {
  const Qwords block = identity_nine();
  std::array<std::uint8_t, 4> rgba = colour(1, 2, 3, 4);

  MeshVertex no_normal;
  require(shade_item_vertex(make_state(LightProgram::nine_qword_first, block), no_normal, rgba) ==
              LightStatus::missing_channel,
          "nine-qword without a normal80 falls back");
  MeshVertex no_colour;
  ItemLightState three = make_state(LightProgram::three_qword_gain, block);
  require(shade_item_vertex(three, no_colour, rgba) == LightStatus::missing_channel,
          "three-qword without a colour falls back");
  MeshVertex pass;
  require(shade_item_vertex(make_state(LightProgram::passthrough, block), pass, rgba) ==
              LightStatus::missing_channel,
          "passthrough without a colour falls back");
  MeshVertex no_raw;
  no_raw.normal80 = {127, 0, 0};
  no_raw.has_normal80 = true;
  ItemLightState six = make_state(LightProgram::six_qword_no_colour, block);
  six.qword[3] = {10.0f, 20.0f, 30.0f, 0.0f};
  require(shade_item_vertex(six, no_raw, rgba) == LightStatus::missing_channel,
          "six-qword without a raw position falls back");
  MeshVertex body;
  require(shade_item_vertex(make_state(LightProgram::body_paint, block), body, rgba) ==
              LightStatus::missing_channel,
          "body without the 0x40 normal and raw position falls back");

  // P == B makes |D| zero; the fragment's RSQRT would be infinite, so the module refuses.
  MeshVertex degenerate;
  degenerate.normal80 = {127, 0, 0};
  degenerate.has_normal80 = true;
  degenerate.raw_position = {10, 20, 30};
  degenerate.has_raw_position = true;
  require(shade_item_vertex(six, degenerate, rgba) == LightStatus::invalid_math,
          "a zero-length reflection direction falls back");
  require(rgba == colour(1, 2, 3, 4), "a refused vertex leaves the colour untouched");
}

void check_status_names() {
  require(std::string(light_program_name(LightProgram::nine_qword_pow8)) == "nine_qword_pow8",
          "the program has a name");
  require(std::string(light_status_name(LightStatus::uncertain_block)) == "uncertain_block",
          "the status has a name");
}

// ---- per-item plumbing ----------------------------------------------------------------------------------------------

void check_shade_mesh_items() {
  CapturedDraw draw;
  draw.item_count = 3;
  // Item 0: the body/paint program, an uploaded six-qword block, base colour 64.
  draw.items[0].run_valid = 1;
  draw.items[0].run_itop = itop_word(0x1058);
  draw.items[0].light_source = kLightItemUploaded;
  draw.items[0].light_block_qwords = 6;
  draw.items[0].light_block_command = kBodyPaintLightBlockCommand;
  const float body_block[6][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0},
                                  {0, 0, 100000.0f, 0}, {1, 1, 1, 1}, {0.8f, 0.8f, 0.8f, 0}};
  for (std::uint32_t qword = 0; qword < 6; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.items[0].light_block[qword][lane] = float_bits(body_block[qword][lane]);
  // Item 1: pass-through (no block read) with a different colour.
  draw.items[1].run_valid = 1;
  draw.items[1].run_itop = itop_word(0x0080);
  draw.items[1].light_source = kLightItemCarriedUncertain;
  // Item 2: the nine-qword first-power program with an uncertain carried block: every vertex falls back.
  draw.items[2].run_valid = 1;
  draw.items[2].run_itop = itop_word(0x0B08);
  draw.items[2].light_source = kLightItemCarriedUncertain;
  draw.items[2].light_block_qwords = 9;
  draw.items[2].light_block_command = kNineQwordLightBlockCommand;

  Mesh mesh;
  mesh.vertices.resize(3);
  // Vertex 0 (item 0): body inputs. N points at LP -> factor 0.2; 64 * 0.2 = 12.8 -> 12.
  mesh.vertices[0].item = 0;
  mesh.vertices[0].normal = {0, 0, 127};
  mesh.vertices[0].has_normal = true;
  mesh.vertices[0].raw_position = {0, 0, 0};
  mesh.vertices[0].has_raw_position = true;
  mesh.vertices[0].color = colour(64, 64, 64, 128);
  mesh.vertices[0].has_color = true;
  // Vertex 1 (item 1): pass-through.
  mesh.vertices[1].item = 1;
  mesh.vertices[1].color = colour(7, 8, 9, 10);
  mesh.vertices[1].has_color = true;
  // Vertex 2 (item 2): a complete nine-qword vertex, but the block is uncertain.
  mesh.vertices[2].item = 2;
  mesh.vertices[2].normal80 = {127, 0, 0};
  mesh.vertices[2].has_normal80 = true;
  mesh.vertices[2].color = colour(50, 60, 70, 80);

  LightCounters counters;
  std::vector<ShadedVertex> shades;
  shade_mesh_items(draw, mesh, counters, &shades);
  require(shades.size() == 3, "one shade record per vertex");
  require(shades[0].program == LightProgram::body_paint && shades[0].status == LightStatus::ok,
          "item 0 uses the body program");
  require(mesh.vertices[0].color == colour(12, 12, 12, 128), "item 0's vertex is lit by its own block");
  require(shades[1].program == LightProgram::passthrough && shades[1].status == LightStatus::ok,
          "item 1 uses the pass-through program");
  require(mesh.vertices[1].color == colour(7, 8, 9, 10), "item 1's vertex is passed through");
  require(shades[2].program == LightProgram::nine_qword_first && shades[2].status == LightStatus::uncertain_block,
          "item 2's uncertain block falls back");
  require(mesh.vertices[2].color == colour(50, 60, 70, 80), "the fallback keeps the base colour");

  require(counters.draws_lit == 1 && counters.draws_fallback == 0, "the draw has lit vertices");
  require(counters.family[static_cast<std::uint32_t>(LightProgram::body_paint)].items_lit == 1,
          "one body item lit");
  require(counters.family[static_cast<std::uint32_t>(LightProgram::passthrough)].items_lit == 1,
          "one pass-through item lit");
  const LightFamilyCounts &nine = counters.family[static_cast<std::uint32_t>(LightProgram::nine_qword_first)];
  require(nine.items == 1 && nine.items_fallback == 1 && nine.vertices_fallback == 1,
          "one nine-qword item fell back");
  require(counters.status[static_cast<std::uint32_t>(LightStatus::uncertain_block)] == 1,
          "the uncertain fallback is counted");
  require(counters.items_uploaded == 1 && counters.items_uncertain == 2,
          "the item light_source tally follows the capture");

  // A second call accumulates: this draws the per-family and global counters exactly once more.
  LightCounters again;
  shade_mesh_items(draw, mesh, again, nullptr);
  LightCounters total;
  total.add(again);
  total.add(counters);
  require(total.draws_lit == 2, "the accumulated draw total");
  require(total.family[static_cast<std::uint32_t>(LightProgram::body_paint)].items_lit == 2,
          "the accumulated family total");
}

}  // namespace

int main() {
  try {
    check_program_selection();
    check_decode_statuses();
    check_nine_first();
    check_nine_powers();
    check_three_qword();
    check_six_qword();
    check_world_family_formulas();
    check_passthrough();
    check_vertex_fallbacks();
    check_status_names();
    check_shade_mesh_items();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 lighting families fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 lighting families fixture: ok\n");
  return 0;
}
