// TASK-090 CoreGS 3.0: the captured post chain on the pixels the live present owns.
//
// The live present draws after CoreGS 2.0's compose, so its fragments skip the game's own post operations: the two
// full-screen multiplies `(Cd * FIX) >> 7` with FIX 127 and FIX 255, and the CT16 present conversion (DTHE/DIMX dither,
// COLCLAMP and the five-bit truncation) that the offline chain models in coregs3_wet_composite.cpp
// (`apply_full_screen_multiply`, `apply_present_ct16`). The fragment stages apply the same operations to the 8-bit
// values they write, through one shared push-constant word (`post`), so the world and the car pass stay consistent with
// the composed base.
//
// The word layout is the contract between this header and the two GLSL stages (coregs3_post.glsl reads it):
//   post.x   output width  (u16 low) | output height (u16 high)
//   post.y   native width  (bits 0-10) | native height (bits 11-21) | flags (bits 22-27)
//   post.z   DIMX low  32 bits (the captured dither matrix)
//   post.w   DIMX high 32 bits
// Flags: bit 0 of (post.y >> 22) enables the chain, bit 1 the faithful CT16 model, bit 2 DTHE, bit 3 COLCLAMP, bit 4
// the FIX 127 multiply, bit 5 the FIX 255 multiply. GT4_COREGS3_POST=0 (A/B) clears the enable; the two multiplies have
// their own measurement switches. The dither model never guesses a register: without a noted frame environment the
// faithful mode is refused and counted by the caller.
// Written from docs/COREGS3-HOOKS.md §28-29 and the dither round, clean room (decision 0070).
#pragma once

#include <cstdint>

namespace gt4::hw::coregs3 {

// The GS dither registers and the native display size at one VBlank, read from the renderer's front end and display
// layout on the stream-owning thread.
struct FrameEnvState {
  std::uint64_t dimx = 0;
  std::uint32_t dthe = 0;
  std::uint32_t colclamp = 1;
  std::uint32_t native_width = 0;
  std::uint32_t native_height = 0;
};

struct PostParameters {
  bool enabled = true;    // GT4_COREGS3_POST (0 disables; the A/B)
  bool fix127 = true;     // GT4_COREGS3_POST_FIX127
  bool fix255 = true;     // GT4_COREGS3_POST_FIX255
  bool faithful = false;  // GT4_COREGS3_DITHER=faithful
  bool dthe = false;
  bool colclamp = true;
  std::uint64_t dimx = 0;
  std::uint32_t native_width = 0;
  std::uint32_t native_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
};

// Packs the push-constant word the two fragment stages read. Sizes above the field widths are clamped, never wrapped
// silently (the caller refuses to draw when a size cannot be represented; the present counts it).
inline void build_post_words(const PostParameters &parameters, std::uint32_t words[4]) noexcept {
  const std::uint32_t output_width = parameters.output_width > 0xFFFFu ? 0xFFFFu : parameters.output_width;
  const std::uint32_t output_height = parameters.output_height > 0xFFFFu ? 0xFFFFu : parameters.output_height;
  const std::uint32_t native_width = parameters.native_width > 0x7FFu ? 0x7FFu : parameters.native_width;
  const std::uint32_t native_height = parameters.native_height > 0x7FFu ? 0x7FFu : parameters.native_height;
  words[0] = (output_width & 0xFFFFu) | (output_height << 16);
  std::uint32_t flags = 0;
  if (parameters.enabled) flags |= 1u << 0;
  if (parameters.faithful) flags |= 1u << 1;
  if (parameters.dthe) flags |= 1u << 2;
  if (parameters.colclamp) flags |= 1u << 3;
  if (parameters.fix127) flags |= 1u << 4;
  if (parameters.fix255) flags |= 1u << 5;
  words[1] = (native_width & 0x7FFu) | ((native_height & 0x7FFu) << 11) | (flags << 22);
  words[2] = static_cast<std::uint32_t>(parameters.dimx & 0xFFFFFFFFu);
  words[3] = static_cast<std::uint32_t>(parameters.dimx >> 32);
}

// `(Cd * FIX) >> 7` with the clamp of apply_full_screen_multiply.
inline std::uint8_t post_multiply(std::uint8_t value, std::uint32_t fix) noexcept {
  const std::uint32_t result = (static_cast<std::uint32_t>(value) * fix) >> 7u;
  return static_cast<std::uint8_t>(result > 255u ? 255u : result);
}

// The captured DIMX entry at an output pixel: the native 4x4 cell it falls in, signed -4..3. A zero native size has
// no defined cell and yields 0 (the shader does the same; the caller refuses the faithful mode when the size is 0).
inline int dither_entry(std::uint64_t dimx, std::uint32_t x, std::uint32_t y, std::uint32_t output_width,
                        std::uint32_t output_height, std::uint32_t native_width, std::uint32_t native_height) noexcept {
  if (output_width == 0u || output_height == 0u || native_width == 0u || native_height == 0u) return 0;
  const std::uint32_t nx = (x * native_width) / output_width;
  const std::uint32_t ny = (y * native_height) / output_height;
  const std::uint32_t entry = 4u * (ny & 3u) + (nx & 3u);
  const std::uint32_t field = static_cast<std::uint32_t>((dimx >> (4u * entry)) & 7u);
  return field >= 4u ? static_cast<int>(field) - 8 : static_cast<int>(field);
}

// The CT16 channel of coregs3_wet_composite.cpp: the COLCLAMP clamp (or the low 8 bits), five bits, expanded by << 3.
inline std::uint8_t ct16_channel(int value, bool clamp_enabled) noexcept {
  if (clamp_enabled) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
  }
  value &= 0xFF;
  return static_cast<std::uint8_t>(((value >> 3) & 0x1F) << 3);
}

}  // namespace gt4::hw::coregs3
