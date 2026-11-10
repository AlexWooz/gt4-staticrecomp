// CTest native_coregs3_lighting: the measured VU1 diffuse lighting formula (native/coregs3/coregs3_lighting.{hpp,cpp}) on
// hand-authored data only. No game data is read.
//
// The expected colours below are the outputs of the real AOT fragment (microcode entry 0x10A0, built by our own VU1
// emitter and driven with the same inputs), not values re-derived from the module under test. They pin:
//   - the formula f = C3 + C4 * min(0, N.U), U = (P - LP)/|P - LP|, in raw model units and with N = bytes / 127;
//   - FTOI0's truncation toward zero (a fractional channel must round down, never to nearest);
//   - the per-lane C3/C4 constants (different lanes can use different slopes);
//   - alpha from lane 3 (the captured block gives exactly the base alpha);
//   - the matrix M never entering the lit colour (the same inputs with a different M must give the same colour);
//   - the fallback statuses: absent/unknown block variants, non-finite values, a vertex without a normal or a raw
//     position, and a zero-length (P - LP).
#include "coregs3_lighting.hpp"

#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

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

LightBlock make_block(const float matrix[3][4], const float lp[4], const float c3[4], const float c4[4]) {
  LightBlock block;
  for (std::uint32_t row = 0; row < 3; ++row)
    for (std::uint32_t lane = 0; lane < 4; ++lane) block.matrix[row][lane] = matrix[row][lane];
  for (std::uint32_t lane = 0; lane < 4; ++lane) {
    block.position[lane] = lp[lane];
    block.c3[lane] = c3[lane];
    block.c4[lane] = c4[lane];
  }
  return block;
}

const float kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
const float kC3[4] = {1.0f, 1.0f, 1.0f, 1.0f};
const float kC4[4] = {0.8f, 0.8f, 0.8f, 0.0f};

std::array<std::int8_t, 3> normal(std::int32_t x, std::int32_t y, std::int32_t z) {
  return {static_cast<std::int8_t>(x), static_cast<std::int8_t>(y), static_cast<std::int8_t>(z)};
}

std::array<std::int32_t, 3> position(std::int32_t x, std::int32_t y, std::int32_t z) { return {x, y, z}; }

std::array<std::uint8_t, 4> colour(std::uint32_t red, std::uint32_t green, std::uint32_t blue, std::uint32_t alpha) {
  return {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green), static_cast<std::uint8_t>(blue),
          static_cast<std::uint8_t>(alpha)};
}

void check_formula() {
  const float lp_front[4] = {0.0f, 0.0f, 10.0f, 0.0f};
  LightBlock block = make_block(kIdentity, lp_front, kC3, kC4);
  std::array<std::uint8_t, 4> rgba{};

  // N points at LP (cos = -1): factor = 1 - 0.8 = 0.2; 64 * 0.2 = 12.8, truncated to 12.
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba),
          "the hand case is shadeable");
  require(rgba == colour(12, 12, 12, 128), "cos = -1 gives the truncated 0.2 factor");

  // The same block with the light off to the side: the cosine changes, the truncation stays toward zero.
  const float lp_side[4] = {100.0f, 50.0f, 10.0f, 0.0f};
  block = make_block(kIdentity, lp_side, kC3, kC4);
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba), "side case");
  require(rgba == colour(59, 59, 59, 128), "an off-axis light shades by the measured cosine");

  // cos = 0 and cos > 0 never brighten: min(0, cos) makes the factor exactly C3.
  const float lp_zero[4] = {10.0f, 0.0f, 0.0f, 0.0f};
  block = make_block(kIdentity, lp_zero, kC3, kC4);
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba), "zero case");
  require(rgba == colour(64, 64, 64, 128), "cos = 0 keeps the constant term");
  const float lp_behind[4] = {0.0f, 0.0f, -10.0f, 0.0f};
  block = make_block(kIdentity, lp_behind, kC3, kC4);
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba), "behind case");
  require(rgba == colour(64, 64, 64, 128), "cos > 0 keeps the constant term");

  // One vertex per position: the fragment's outputs for LP = (0,0,10) and N = +z. The third value of each row is the
  // fractional-channel truncation check (64 * 0.2 = 12.8 -> 12, never 13).
  block = make_block(kIdentity, lp_front, kC3, kC4);
  const std::int32_t positions[8][3] = {{-35, 0, 0},  {-25, 0, 5}, {-15, 0, 10}, {-5, 0, 0},
                                        {5, 0, 5},    {15, 0, 10}, {25, 0, 0},  {35, 0, 5}};
  const std::uint8_t expected[8] = {49, 53, 64, 18, 27, 64, 44, 56};
  for (std::uint32_t index = 0; index < 8; ++index) {
    require(shade_vertex(block, normal(0, 0, 127),
                         position(positions[index][0], positions[index][1], positions[index][2]),
                         colour(64, 64, 64, 128), rgba),
            "the varied positions are shadeable");
    require(rgba == colour(expected[index], expected[index], expected[index], 128),
            "the varied positions match the fragment");
  }

  // Per-lane constants: C4 = (0.1, 0.5, 0.9) and N = (73,73,73)/127. Every channel has its own slope.
  const float c4_lanes[4] = {0.1f, 0.5f, 0.9f, 0.0f};
  const float lp_far_x[4] = {100.0f, 0.0f, 0.0f, 0.0f};
  block = make_block(kIdentity, lp_far_x, kC3, c4_lanes);
  require(shade_vertex(block, normal(73, 73, 73), position(0, 0, 0), colour(100, 100, 100, 128), rgba),
          "the per-lane case is shadeable");
  require(rgba == colour(94, 71, 48, 128), "C3/C4 are applied per lane");

  // Alpha comes from lane 3: the captured block has C3.w = 1 and C4.w = 0, so it is exactly the base alpha.
  block = make_block(kIdentity, lp_far_x, kC3, kC4);
  require(shade_vertex(block, normal(127, 0, 0), position(0, 0, 0), colour(64, 64, 64, 200), rgba), "alpha case");
  require(rgba == colour(12, 12, 12, 200), "alpha keeps the base value with C3.w = 1, C4.w = 0");
  const float c4_alpha[4] = {0.8f, 0.8f, 0.8f, 0.5f};
  block = make_block(kIdentity, lp_front, kC3, c4_alpha);
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 200), rgba),
          "the lane-3 case is shadeable");
  require(rgba == colour(12, 12, 12, 100), "C4.w scales the alpha lane like every other lane");

  // A non-axis normal against a realistic captured-style light position, from the fragment's own output.
  const float lp_model[4] = {29557.0f, 45723.0f, 602217.0f, 0.0f};
  block = make_block(kIdentity, lp_model, kC3, kC4);
  require(shade_vertex(block, normal(73, -51, 89), position(1000, -2000, 300), colour(64, 128, 32, 255), rgba),
          "the captured-style case is shadeable");
  require(rgba == colour(28, 57, 14, 255), "the captured-style case matches the fragment");
}

// The fragment uses M for a separate output; the lit colour must not depend on it. The expected values are the identity
// matrix's outputs above.
void check_matrix_is_not_used() {
  const float lp_front[4] = {0.0f, 0.0f, 10.0f, 0.0f};
  const float matrix[3][4] = {{0.3f, 0.5f, -0.2f, 0.0f}, {0.1f, -0.9f, 0.4f, 0.0f}, {0.7f, 0.2f, 0.6f, 0.0f}};
  const LightBlock block = make_block(matrix, lp_front, kC3, kC4);
  std::array<std::uint8_t, 4> rgba{};
  require(shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba),
          "the M-varied case is shadeable");
  require(rgba == colour(12, 12, 12, 128), "M does not enter the lit colour");
}

void check_mesh_vertex_overload() {
  const float lp_front[4] = {0.0f, 0.0f, 10.0f, 0.0f};
  const LightBlock block = make_block(kIdentity, lp_front, kC3, kC4);
  MeshVertex vertex;
  vertex.normal = normal(0, 0, 127);
  vertex.raw_position = position(0, 0, 0);
  vertex.color = colour(64, 64, 64, 128);
  vertex.has_normal = true;
  vertex.has_raw_position = true;
  std::array<std::uint8_t, 4> rgba{};
  require(shade_vertex(block, vertex, rgba), "a complete MeshVertex is shadeable");
  require(rgba == colour(12, 12, 12, 128), "the MeshVertex overload is the component formula");
  vertex.has_normal = false;
  require(!shade_vertex(block, vertex, rgba), "a vertex without a normal falls back");
  vertex.has_normal = true;
  vertex.has_raw_position = false;
  require(!shade_vertex(block, vertex, rgba), "a vertex without a raw position falls back");
}

void check_fallbacks() {
  const float lp_front[4] = {0.0f, 0.0f, 10.0f, 0.0f};
  LightBlock block = make_block(kIdentity, lp_front, kC3, kC4);
  std::array<std::uint8_t, 4> rgba = colour(1, 2, 3, 4);
  // P == LP gives a zero-length direction; the fragment's EFU would divide by zero, so the module refuses instead of
  // inventing a colour.
  require(!shade_vertex(block, normal(0, 0, 127), position(0, 0, 10), colour(64, 64, 64, 128), rgba),
          "P == LP falls back");
  require(rgba == colour(1, 2, 3, 4), "a refused vertex leaves the output untouched");
  // A non-finite light position is refused by the same guard (a hand-built block can carry one).
  block.position[2] = std::numeric_limits<float>::infinity();
  require(!shade_vertex(block, normal(0, 0, 127), position(0, 0, 0), colour(64, 64, 64, 128), rgba),
          "a non-finite direction falls back");
  // A zero normal is valid: cos = 0 and the factor is C3 (the fragment does the same).
  block = make_block(kIdentity, lp_front, kC3, kC4);
  require(shade_vertex(block, normal(0, 0, 0), position(0, 0, 0), colour(64, 64, 64, 128), rgba),
          "a zero normal is the constant term");
  require(rgba == colour(64, 64, 64, 128), "a zero normal gives C3 exactly");
}

void check_decode() {
  CapturedDraw draw;
  LightBlock block;
  require(decode_light_block(draw, block) == LightBlockStatus::missing, "no block is missing");

  draw.light_block_qwords = kBodyPaintLightBlockQwords;
  draw.light_block_command = kBodyPaintLightBlockCommand;
  for (std::uint32_t qword = 0; qword < kBodyPaintLightBlockQwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.light_block[qword][lane] = float_bits(static_cast<float>(qword * 4 + lane));
  require(decode_light_block(draw, block) == LightBlockStatus::ok, "the measured variant decodes");
  require(block.matrix[1][2] == 6.0f && block.position[1] == 13.0f && block.c3[3] == 19.0f &&
              block.c4[0] == 20.0f,
          "the six qwords land in M, LP, C3 and C4");

  // The qword count is part of the variant: a nine-qword upload is a different shape call, not this block.
  CapturedDraw other = draw;
  other.light_block_qwords = 9;
  other.light_block_command = 0x6C09C0C9u;
  require(decode_light_block(other, block) == LightBlockStatus::unknown_variant, "the nine-qword variant is refused");
  other = draw;
  other.light_block_command = 0x6C06C0C8u;  // same count, a different destination
  require(decode_light_block(other, block) == LightBlockStatus::unknown_variant, "a different destination is refused");

  // A non-finite qword is refused rather than propagated.
  other = draw;
  other.light_block[3][1] = 0x7FC00000u;  // NaN
  require(decode_light_block(other, block) == LightBlockStatus::not_finite, "a NaN qword is refused");

  require(std::string(light_block_status_name(LightBlockStatus::ok)) == "ok", "the status has a name");
  require(std::string(light_block_status_name(LightBlockStatus::unknown_variant)) == "unknown_variant",
          "the variant status has a name");
}

}  // namespace

int main() {
  try {
    check_formula();
    check_matrix_is_not_used();
    check_mesh_vertex_overload();
    check_fallbacks();
    check_decode();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 lighting fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 lighting fixture: ok\n");
  return 0;
}
