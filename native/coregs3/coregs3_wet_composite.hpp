// TASK-090 CoreGS 3.0, phase S2: the offline wet-road colour composite and the captured post multiplies.
//
// The S2 census (coregs3_gsdecode.hpp) identifies the chain in the PATH2 stream of scene frame V (stamped V + 1):
//   * a full-screen alpha clear of the main frame (FRAME FBP 210, FBMSK 0x00FFFFFF, RGBA alpha 0);
//   * a full-screen additive composite that samples the sphere-mapper target (TEX0 TBP0 0x3140, CT32 128x128, region
//     clamp 0..127, linear filtering) with the captured sprite vertices and vertex colour (64,64,64,128), blending
//     `(Cs - 0) * Ad + Cd` and writing both colour and alpha;
//   * two full-screen multiplies of the frame, `Cd * FIX / 128` with FIX 127 and FIX 255 (the captured post chain's
//     darken and brighten operations).
// This unit applies those draws to an already rendered world image. The source of the composite is the CT32 page the
// capture's per-job `.gt4vram` record holds for the sphere-mapper job (captured before that job executes, which is the
// exact stream position the composite samples); the destination alpha is the framebuffer alpha after the alpha clear
// and the captured reflection pass's alpha-only draws. Nothing is inferred: the caller supplies both, and a draw outside
// the measured signature is refused with a reason.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_gsdecode.hpp"

namespace gt4::coregs3 {

struct WetCompositeStats {
  std::uint64_t sprites = 0;
  std::uint64_t pixels = 0;          // output pixels the sprites cover
  std::uint64_t masked_pixels = 0;   // covered pixels whose destination alpha (Ad) is non-zero
  std::uint64_t changed_pixels = 0;  // covered pixels whose RGB changed
  std::uint64_t sampled_pixels = 0;  // successful texture samples
  std::uint64_t refused_pixels = 0;  // sampler refusals (never silently filled)
  std::uint64_t alpha_written = 0;   // pixels whose alpha lane was written
  double added_luma = 0.0;           // sum of (new luma - old luma) over the covered pixels, for the report
};

// Applies one composite batch (coregs3_gsdecode.hpp) to `rgba` at `width` x `height`, with the captured screen
// coordinates in `capture_width` x `capture_height` (640 x 448) scaled to the output. `source_rgba` is the CT32 texture
// page (source_width x source_height, row-major, top row first). Returns false with `error` when the batch is not the
// measured composite: a different PRIM/TEX0/TEX1/CLAMP/ALPHA state, a vertex run that is not latch/draw sprite pairs, or
// a missing UV.
bool apply_wet_composite(const GsDecodeBatch &draw, const std::uint8_t *source_rgba, std::uint32_t source_width,
                         std::uint32_t source_height, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                         std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                         WetCompositeStats &stats, std::string &error,
                         std::vector<std::uint8_t> *covered_mask = nullptr,
                         std::vector<std::uint8_t> *alpha_mask = nullptr);

// Applies one full-screen multiply batch: `(Cd - 0) * FIX / 128` per RGB channel (arithmetic shift, clamp 255) and the
// written alpha is the captured RGBA alpha (As). Only the measured `(Cd, Zero, FIX, Zero)` ALPHA equation is accepted.
bool apply_full_screen_multiply(const GsDecodeBatch &draw, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                                std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                                WetCompositeStats &stats, std::string &error);

}  // namespace gt4::coregs3
