// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-205 measurements (docs/SHADER-VARIANT-COVERAGE.md) and decision 0079, clean room.
//
// TASK-206: the render scale is part of the CoreGS 2.0 pipeline key (TASK-205), so a session at a scale the shipped list
// never recorded asks for a parallel set of every draw state it touches (the player's 44 new keys in one lap at 4x). This
// is the prepare-time list transformation that covers them: the shipped `.inc` stays as it is, nothing changes in the
// key format, the shader or the pipeline specialisation.
//
// A key is 33 words: the 30 draw-state fields, the push word, the mip anisotropy and the ROP word. The push word holds
//  - bits 0-19: the sampled texture image's scale: 1 for the atlas, a replacement image, a mipmapped game texture and a
//    1x effect target; the frame's scale for a target drawn at the session scale (the main frame included);
//  - bit 24: the batch samples a texture image (an atlas or a drawn target); always clear when the draw is untextured,
//    because spec_key forces the push word of an untextured draw to 1;
//  - bit 28 (depth from position) and bit 30 (fine dither): both come from `frame->scale > 1`, not from the texture, and
//    spec_key clears bit 28 when the draw has no depth test and bit 30 when it does not dither.
// So a key of a session at scale S has one of two shapes: the sampled image at 1x (`1`), or a target at S (`S`), both with
// the frame's two flags. `variant_scaled_keys` writes both shapes for a recorded key: that is what a session at S can ask
// for, and deduplication collapses them when S is 1 (both shapes are the same key there).
#pragma once

#include <cstddef>
#include <cstdint>

namespace hwgs::core2 {

// The words of a variant key (core2_gpu.cpp SpecKey): the 30 draw-state fields, the push word, the anisotropy, the ROP word.
inline constexpr std::size_t kVariantKeyWords = 33;
inline constexpr std::size_t kVariantPushSlot = 30;
// Key slots of the two fields the frame flags depend on: Dither (index 2) and Ztest (index 8) of kSpecFields.
inline constexpr std::size_t kVariantDitherSlot = 2;
inline constexpr std::size_t kVariantZtestSlot = 8;

inline constexpr std::uint32_t kPushSampledScaleMask = 0x000FFFFFu;   // bits 0-19: the sampled texture's scale
inline constexpr std::uint32_t kPushTexturedImage = 0x01000000u;      // bit 24: the batch samples a texture image
inline constexpr std::uint32_t kPushDepthFromPosition = 0x10000000u;  // bit 28
inline constexpr std::uint32_t kPushDitherFine = 0x40000000u;         // bit 30
inline constexpr std::uint32_t kPushFrameScaleMask = kPushDepthFromPosition | kPushDitherFine;

// The push word of `key` for a frame at `frameScale` sampling an image at `textureScale`, keeping every other push bit.
// The two frame flags follow the frame, not the texture: set above 1x and only where spec_key keeps them (a draw that
// tests depth keeps bit 28, a draw that dithers keeps bit 30).
inline std::uint32_t variant_push_word(const std::uint32_t *key, std::uint32_t frameScale, std::uint32_t textureScale) {
  std::uint32_t flags = 0;
  if (frameScale > 1) {
    if (key[kVariantZtestSlot] != 0) flags |= kPushDepthFromPosition;
    if (key[kVariantDitherSlot] != 0) flags |= kPushDitherFine;
  }
  const std::uint32_t push = key[kVariantPushSlot] & ~(kPushSampledScaleMask | kPushFrameScaleMask);
  return push | (textureScale & kPushSampledScaleMask) | flags;
}

// The keys a session at `frameScale` can ask for `key` (recorded at any scale). One key always: the sampled image at 1x
// (the atlas, a replacement image, a mipmapped game texture or a 1x effect target). A second one when the key samples a
// texture image and the frame is above 1x: that image as a target drawn at the frame's scale. `out` holds two keys and
// the return value says how many were written (1 or 2); at 1x the two shapes are one key.
inline std::size_t variant_scaled_keys(const std::uint32_t *key, std::uint32_t frameScale,
                                       std::uint32_t out[2][kVariantKeyWords]) {
  for (std::size_t word = 0; word < kVariantKeyWords; ++word) out[0][word] = key[word];
  out[0][kVariantPushSlot] = variant_push_word(key, frameScale, 1);
  if (frameScale <= 1 || (key[kVariantPushSlot] & kPushTexturedImage) == 0) return 1;
  for (std::size_t word = 0; word < kVariantKeyWords; ++word) out[1][word] = key[word];
  out[1][kVariantPushSlot] = variant_push_word(key, frameScale, frameScale);
  return 2;
}

}  // namespace hwgs::core2
