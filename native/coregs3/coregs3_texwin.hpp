// TASK-090 CoreGS 3.0: the GS blocks one TEX0 binds, shared by both capture sides.
//
// The `.gt4vram` snapshot kinds that carry texture pixels need the same block list on the EE (the K1 draw marker) and on
// the GS worker (the per-job snapshot at the draw's VU1 job). This header keeps the rule in one place: the texture's page
// run from TBP0/TBW/TW/TH/PSM (T4 128x128, T8 128x64, CT32 64x32 texels, 8 KiB per page, so 32 blocks per page) plus
// the CSM1 palette's four blocks at CBP. The run is contiguous from TBP0 and may include padding pages when TBW is wider
// than the texture; the bounds keep a malformed record from asking for a huge copy.
#pragma once

#include <cstdint>

#include "coregs3_vram.hpp"

namespace gt4::coregs3 {

inline constexpr std::uint32_t kMaxTextureWindowBlocks = 512u;

struct TexturePageGeometry {
  std::uint32_t width;
  std::uint32_t height;
};

inline TexturePageGeometry page_geometry_from_psm(std::uint32_t psm) noexcept {
  if (psm == 0x14u) return {128u, 128u};  // T4
  if (psm == 0x13u) return {128u, 64u};   // T8
  return {64u, 32u};                      // CT32 (and the formats the decoder refuses)
}

inline void add_texture_window(vram::Window *windows, std::uint32_t &count, std::uint32_t first,
                               std::uint32_t blocks) noexcept {
  if (first >= vram::kBlocksTotal || blocks == 0) return;
  if (blocks > vram::kBlocksTotal - first) blocks = vram::kBlocksTotal - first;
  if (blocks > kMaxTextureWindowBlocks) blocks = kMaxTextureWindowBlocks;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (windows[index].first_block == first) {
      if (blocks > windows[index].block_count) windows[index].block_count = blocks;
      return;
    }
  }
  if (count >= vram::kMaxCaptureWindows) return;
  windows[count].first_block = first;
  windows[count].block_count = blocks;
  ++count;
}

// Appends the windows a TEX0 word binds. Only the three decoded formats are covered; anything else binds nothing (the
// decoder refuses it).
inline void texture_windows_from_tex0(std::uint64_t tex0, vram::Window *windows, std::uint32_t &count) noexcept {
  const std::uint32_t tbp0 = static_cast<std::uint32_t>(tex0 & 0x3FFFu);
  const std::uint32_t tbw = static_cast<std::uint32_t>((tex0 >> 14) & 0xFu);
  const std::uint32_t psm = static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu);
  const std::uint32_t tw = static_cast<std::uint32_t>((tex0 >> 26) & 0xFu);
  const std::uint32_t th = static_cast<std::uint32_t>((tex0 >> 30) & 0xFu);
  const std::uint32_t cbp = static_cast<std::uint32_t>((tex0 >> 37) & 0x3FFFu);
  if (psm != 0x00u && psm != 0x13u && psm != 0x14u) return;
  const TexturePageGeometry geometry = page_geometry_from_psm(psm);
  const std::uint32_t width = 1u << tw;
  const std::uint32_t height = 1u << th;
  const std::uint32_t pagesPerRow = tbw * 64u / geometry.width > 0u ? tbw * 64u / geometry.width : 1u;
  const std::uint32_t pagesY = (height + geometry.height - 1u) / geometry.height;
  add_texture_window(windows, count, tbp0, 32u * pagesPerRow * pagesY);
  if (psm == 0x13u || psm == 0x14u) add_texture_window(windows, count, cbp, 4u);  // CSM1 palette bank
}

}  // namespace gt4::coregs3
