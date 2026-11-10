// TASK-090 CoreGS 3.0: the measured VU1 lighting formula, shared by the offline reference renderer
// (native/coregs3/coregs3_render.cpp) and the live `present` layer (native/hw_gs/coregs3/hw_coregs3_present.cpp).
//
// The VU1 lighting fragment (microcode entry 0x10A0, the body/paint variant) reads, per vertex:
//   P = the model position at TOPS+0x00 after ITOF0: the raw signed 16-bit V3-16 value as a float, in RAW MODEL
//       UNITS. No position_scale division is applied: LP is in the same raw unit space.
//   N = the V3-8 normal at TOPS+0x40, sign-extended and divided by 127 (the model stores a unit normal scaled to the
//       int8 byte range).
//   B = the V4-8 base colour at TOPS+0x80, 0..255 per lane.
// and the six-qword light block the shape call uploads at TOPS+0xC9:
//   0xC9..0xCB   M, a 3x3 matrix (one qword per row);
//   0xCC         LP, the light position, four floats, in the same raw model unit space as P;
//   0xCD         C3 = (c3,c3,c3,1), the constant term;
//   0xCE         C4 = (c4,c4,c4,0), the diffuse slope (0.8 in the measured block, so it only darkens).
// The fragment computes, in float32 and in this exact order:
//   D      = P - LP
//   cos    = (D.x*N.x + D.y*N.y) + D.z*N.z          (lane 0 of D*N, summed left to right)
//   t      = min(cos, 0) * (1 / |D|)                 (min first, then the reciprocal length)
//   factor = C3 + C4 * t                             (per lane)
//   lit    = FTOI0(B * factor)                       (truncate toward zero, per lane)
//   alpha  = FTOI0(B.w * (C3.w + C4.w * t))          (the captured block gives exactly B.w)
//
// M does NOT enter the lit colour. The fragment uses M to transform the reflected vector D - 2*N*(D.N) into a separate
// output (the two floats it writes at TOPS+0x40); varying M with every other input fixed leaves the colour unchanged,
// which is how its role was measured. Only the measured six-qword body/paint variant is accepted here (command
// 0x6C06C0C9). Every other block variant is unknown to this module: the caller falls back to the base colour and
// counts it, never invents constants.
//
// Evidence (round 11, TASKS.md TASK-090): driving the real AOT fragment with the captured block reproduces the tapped
// RGBAQ on 3,822 unambiguous body-family vertices with max 1 level per channel and alpha exact; draws 91/93 are a
// separate sub-family whose tap pairing is ambiguous. The same float32 formula, run on the same rows, matches the
// fragment exactly (3,914 of 3,914 rows, RGB and alpha).
#pragma once

#include <array>
#include <cstdint>

namespace gt4::coregs3 {

// Values of the six-qword body/paint block, as the capture records them. The command is the tag's VIF UNPACK word:
// V4-32 (0x6C), NUM = 6 qwords, destination TOPS+0xC9. The qword count is part of the variant: a file that carries a
// different count is a different upload, not this one.
inline constexpr std::uint32_t kBodyPaintLightBlockCommand = 0x6C06C0C9u;
inline constexpr std::uint32_t kBodyPaintLightBlockQwords = 6u;

// The decoded light block. Raw float values, exactly as the VU1 received them.
struct LightBlock {
  std::array<float, 4> matrix[3]{};  // M, qwords 0xC9..0xCB; not part of the lit colour
  std::array<float, 4> position{};   // LP, qword 0xCC
  std::array<float, 4> c3{};         // C3, qword 0xCD
  std::array<float, 4> c4{};         // C4, qword 0xCE
};

enum class LightBlockStatus : std::uint8_t {
  ok = 0,
  missing,          // the draw carried no light block upload (light_block_qwords == 0)
  unknown_variant,  // a block is present but it is not the measured six-qword body/paint form
  not_finite,       // one of the qword values is NaN or infinite
};
const char *light_block_status_name(LightBlockStatus status) noexcept;

// Reads the light block of one draw. Returns ok and fills `block` only for the measured variant; for every other
// status `block` is left untouched and the caller must fall back.
LightBlockStatus decode_light_block(const struct CapturedDraw &draw, LightBlock &block) noexcept;

// Shades one vertex with the captured block. `vertex` must carry the V3-8 normal and the raw V3-16 position
// (MeshVertex::normal / raw_position); `rgba` receives the lit colour. Returns false and leaves `rgba` untouched when
// the vertex cannot be shaded with this block: no normal, no raw position, a zero/non-finite (P - LP), or a non-finite
// result. The caller then keeps the base colour and counts the fallback.
bool shade_vertex(const LightBlock &block, const struct MeshVertex &vertex,
                  std::array<std::uint8_t, 4> &rgba) noexcept;

// The same formula without a MeshVertex, for tools and fixtures. `normal` is the raw int8 triplet, `raw_position` the
// signed integer the VU1 converts with ITOF0, `base` the V4-8 colour.
bool shade_vertex(const LightBlock &block, const std::array<std::int8_t, 3> &normal,
                  const std::array<std::int32_t, 3> &raw_position, const std::array<std::uint8_t, 4> &base,
                  std::array<std::uint8_t, 4> &rgba) noexcept;

}  // namespace gt4::coregs3
