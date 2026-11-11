// TASK-090 CoreGS 3.0: the GS blocks one TEX0 binds, shared by both capture sides.
//
// The `.gt4vram` snapshot kinds that carry texture pixels need the same block list on the EE (the K1 draw marker) and on
// the GS worker (the per-job snapshot at the draw's VU1 job). This header keeps the rule in one place: the texture's page
// run from TBP0/TBW/TW/TH/PSM (T4 128x128, T8 128x64, CT32 64x32 texels, 8 KiB per page, so 32 blocks per page) plus
// the CSM1 palette's four blocks at CBP. The run is contiguous from TBP0 and may include padding pages when TBW is wider
// than the texture; the bounds keep a malformed record from asking for a huge copy.
#pragma once

#include <algorithm>
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

// TASK-090 texture mip: the TEX1 fields the mip level addresses come from (chapter 01.5): MXL bits 2-4 and MTBA bit 9.
inline std::uint32_t tex1_mxl(std::uint64_t tex1) noexcept { return static_cast<std::uint32_t>((tex1 >> 2) & 7u); }
inline bool tex1_mtba(std::uint64_t tex1) noexcept { return ((tex1 >> 9) & 1u) != 0u; }

// Blocks of one pixel at a PSM (chapter 01.2): 0 for a format the capture does not decode.
inline std::uint32_t psm_bits_per_pixel(std::uint32_t psm) noexcept {
  switch (psm) {
    case 0x00u: return 32u;  // CT32
    case 0x13u: return 8u;   // T8
    case 0x14u: return 4u;   // T4
    default: return 0u;
  }
}

// TASK-090 texture mip: the block/TBW address of mip level 1..6. This repeats the hw_gs_lod rules (MIPTBP1/2 groups and
// the MTBA packed layout) because the capture units do not link that module; the texture-mip fixture asserts the two
// agree on the same vectors. The caller has already checked MXL and the PSM.
inline void mip_level_address(std::uint32_t level, std::uint64_t tex0, std::uint64_t tex1, std::uint64_t miptbp1,
                              std::uint64_t miptbp2, std::uint32_t &tbp, std::uint32_t &tbw) noexcept {
  const std::uint32_t tbp0 = static_cast<std::uint32_t>(tex0 & 0x3FFFu);
  const std::uint32_t tbw0 = static_cast<std::uint32_t>((tex0 >> 14) & 0x3Fu);
  const std::uint32_t psm = static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu);
  const std::uint32_t tw = static_cast<std::uint32_t>((tex0 >> 26) & 0x0Fu);
  const std::uint32_t bits = psm_bits_per_pixel(psm);
  const bool is_32_bit = psm == 0x00u;
  const std::uint32_t maximum_tw = is_32_bit ? 9u : 10u;
  if (tex1_mtba(tex1) && bits != 0u && tw >= 5u && tw <= maximum_tw) {
    // MTBA: level n starts after the block count of level n-1 (blocks0 / 4^(n-1)); TBW halves per level.
    const std::uint64_t texels = std::uint64_t{1} << tw;
    const std::uint64_t blocks0 = texels * texels * bits / 2048u;
    const std::uint64_t bw0 = std::max<std::uint64_t>(1u, texels / 64u);
    std::uint64_t address = tbp0;
    std::uint64_t previous = blocks0;
    for (std::uint32_t n = 1; n <= level; ++n) {
      address += previous;
      previous = std::max<std::uint64_t>(1u, previous / 4u);
    }
    tbp = static_cast<std::uint32_t>(address);
    tbw = static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, bw0 >> level));
    return;
  }
  const std::uint64_t word = level <= 3u ? miptbp1 : miptbp2;
  const unsigned shift = 20u * ((level <= 3u ? level - 1u : level - 4u));
  tbp = static_cast<std::uint32_t>((word >> shift) & 0x3FFFu);
  tbw = static_cast<std::uint32_t>((word >> (shift + 14u)) & 0x3Fu);
}

// TASK-090 texture mip: appends the page runs of levels 1..MXL a TEX0/TEX1 pair reads. A capture that carries only the
// TEX0 run cannot see the mip content: at the measured road draw the MIPTBP level address holds a different texture's
// page, and the job snapshot's frame-end fallback is a later upload. Including the levels is what makes a future
// `.gt4vram` carry the chain the decoder samples.
inline void texture_mip_windows(std::uint64_t tex0, std::uint64_t tex1, std::uint64_t miptbp1, std::uint64_t miptbp2,
                                vram::Window *windows, std::uint32_t &count) noexcept {
  const std::uint32_t psm = static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu);
  const std::uint32_t mxl = tex1_mxl(tex1);
  if (mxl == 0u || mxl > 6u || psm_bits_per_pixel(psm) == 0u) return;
  const std::uint32_t tw = static_cast<std::uint32_t>((tex0 >> 26) & 0x0Fu);
  const std::uint32_t th = static_cast<std::uint32_t>((tex0 >> 30) & 0x0Fu);
  const TexturePageGeometry geometry = page_geometry_from_psm(psm);
  for (std::uint32_t level = 1; level <= mxl; ++level) {
    std::uint32_t tbp = 0, tbw = 0;
    mip_level_address(level, tex0, tex1, miptbp1, miptbp2, tbp, tbw);
    const std::uint32_t width = std::max<std::uint32_t>(1u, (1u << tw) >> level);
    const std::uint32_t height = std::max<std::uint32_t>(1u, (1u << th) >> level);
    const std::uint32_t pagesPerRow = tbw * 64u / geometry.width > 0u ? tbw * 64u / geometry.width : 1u;
    const std::uint32_t pagesY = (height + geometry.height - 1u) / geometry.height;
    add_texture_window(windows, count, tbp, 32u * pagesPerRow * pagesY);
  }
}

// TASK-090 texture mip: the TEX0 run, the palette bank and every mip level the TEX1.MXL asks for.
inline void texture_windows_from_state(std::uint64_t tex0, std::uint64_t tex1, std::uint64_t miptbp1,
                                       std::uint64_t miptbp2, vram::Window *windows,
                                       std::uint32_t &count) noexcept {
  texture_windows_from_tex0(tex0, windows, count);
  texture_mip_windows(tex0, tex1, miptbp1, miptbp2, windows, count);
}

}  // namespace gt4::coregs3
