// TASK-090 CoreGS 3.0, phase S3: the first native HUD/2D layer. See coregs3_hud.hpp for the measured contract.
#include "coregs3_hud.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <tuple>

#include "coregs3_gif.hpp"
#include "coregs3_sampler.hpp"

namespace gt4::coregs3 {
namespace {

// ---- Small helpers ----

std::uint64_t bits(std::uint64_t value, unsigned low, unsigned high) {
  return (value >> low) & ((std::uint64_t{1} << (high - low + 1u)) - 1u);
}

// The measured 12.4 wrap rule of the S2 walker and the tap: dx = (x - ofx) & 0xFFFF, sx = dx / 16, then the signed
// 12-bit range.
float screen_coordinate(std::uint32_t raw, std::uint32_t offset) {
  const std::uint32_t delta = (raw - offset) & 0xFFFFu;
  float value = static_cast<float>(delta) / 16.0f;
  if (value > 2047.9375f) value -= 4096.0f;
  return value;
}

std::uint32_t prim_type(std::uint32_t prim) noexcept { return prim & 7u; }
bool prim_tme(std::uint32_t prim) noexcept { return ((prim >> 4) & 1u) != 0u; }
bool prim_abe(std::uint32_t prim) noexcept { return ((prim >> 6) & 1u) != 0u; }

// Half-open sprite/strip coverage is expressed by the caller in canvas pixels; the output mapping (TASK-214) turns each
// target pixel centre into a canvas coordinate before this rule applies.

std::uint8_t ct16_store(std::uint8_t value) noexcept { return static_cast<std::uint8_t>((value >> 3) << 3); }

// The scissor rectangle of a canonical SCISSOR word.
struct ScissorRect {
  std::int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  bool empty() const noexcept { return x1 < x0 || y1 < y0; }
  bool contains(float min_x, float max_x, float min_y, float max_y) const noexcept {
    return min_x >= static_cast<float>(x0) && max_x <= static_cast<float>(x1) + 1.0f &&
           min_y >= static_cast<float>(y0) && max_y <= static_cast<float>(y1) + 1.0f;
  }
};
ScissorRect scissor_rect(std::uint64_t scissor) {
  ScissorRect rect;
  rect.x0 = static_cast<std::int32_t>(bits(scissor, 0, 10));
  rect.x1 = static_cast<std::int32_t>(bits(scissor, 16, 26));
  rect.y0 = static_cast<std::int32_t>(bits(scissor, 32, 42));
  rect.y1 = static_cast<std::int32_t>(bits(scissor, 48, 58));
  return rect;
}

// The tap's register file, tracked in execution order. Only the registers the HUD uses are kept.
struct TapState {
  std::uint64_t prim = 0;
  std::uint64_t frame = 0;
  std::uint64_t zbuf = 0;
  std::uint64_t test = 0;
  std::uint64_t alpha = 0;
  std::uint64_t scissor = 0;
  std::uint64_t xyoffset = 0;
  std::uint64_t tex0 = 0;
  std::uint64_t tex1 = 0;
  std::uint64_t clamp = 0;
  bool frame_known = false;
};

void apply_tap_register(TapState &state, std::uint32_t address, std::uint64_t data) {
  switch (address & 0x7Fu) {
    case 0x00: state.prim = data & 0x7FFu; break;
    case 0x06:
    case 0x07: state.tex0 = data; break;
    case 0x08:
    case 0x09: state.clamp = data; break;
    case 0x14:
    case 0x15: state.tex1 = data; break;
    case 0x18:
    case 0x19: state.xyoffset = data; break;
    case 0x40:
    case 0x41: state.scissor = data; break;
    case 0x42:
    case 0x43: state.alpha = data; break;
    case 0x47:
    case 0x48: state.test = data; break;
    case 0x4C:
    case 0x4D:
      state.frame = data;
      state.frame_known = true;
      break;
    case 0x4E:
    case 0x4F: state.zbuf = data; break;
    default: break;
  }
}

// The measured speed-element placement and state. The HUD atlas pages rotate through VRAM between frames (the same
// element lands at TBP0 0x3A00 in stream 7398 and 0x3A20 in streams 7399..7405), so the selection uses the element's
// measured CLAMP regions and screen placement instead of one page number. The plate is the only T8 region of the HUD
// (0..81 x 0..25); the digits are the T4 glyphs of the font atlas (0..127 x 0..38) inside the plate's rectangle.
constexpr std::uint32_t kHudFirstTbp0 = 0x3800u;
constexpr std::uint32_t kHudLastTbp0 = 0x3EFFu;

HudDrawState from_decode_state(const GsDecodeState &state) {
  HudDrawState out;
  out.prim = state.prim_value();
  out.frame = state.frame;
  out.zbuf = state.zbuf;
  out.test = state.test;
  out.alpha = state.alpha;
  out.scissor = state.scissor;
  out.xyoffset = state.xyoffset;
  out.tex0 = state.tex0;
  out.tex1 = state.tex1;
  out.clamp = state.clamp;
  out.rgba = static_cast<std::uint32_t>(state.rgba & 0xFFFFFFFFu);
  return out;
}

// Selects the panel's PATH2 sprites for the element. Returns true when the batch is one of the element's sprites.
bool panel_path2_matches(const GsDecodeBatch &batch, HudElement element) {
  if (element != HudElement::speed) return false;
  if (batch.path != 2 || !prim_tme(batch.state.prim_value()) || prim_type(batch.state.prim_value()) != 6) return false;
  if (batch.state.frame_psm() != 2u) return false;
  const std::uint32_t tbp0 = batch.state.tex0_tbp0();
  if (tbp0 < kHudFirstTbp0 || tbp0 > kHudLastTbp0 || batch.state.tex0_psm() != 0x14u) return false;
  if (batch.state.clamp_maxu() != 127u || batch.state.clamp_maxv() != 38u) return false;
  // The measured placement: both digits lie inside the plate's rectangle (x 567.75..588.0, y 382.5..400.5).
  return batch.min_x >= 560.0f && batch.max_x <= 595.0f && batch.min_y >= 378.0f && batch.max_y <= 404.0f;
}

// The census-driven selection: every panel sprite the measured raster can draw (a textured, axis-aligned pair of a T4
// or T8 page into the CT16 display buffer). The element-specific placement filters are not applied.
bool panel_path2_matches_all(const GsDecodeBatch &batch) {
  if (batch.path != 2 || !prim_tme(batch.state.prim_value()) || prim_type(batch.state.prim_value()) != 6) return false;
  if (batch.state.frame_psm() != 2u) return false;
  const std::uint32_t psm = batch.state.tex0_psm();
  if (psm != 0x14u && psm != 0x13u) return false;
  if (batch.verts.size() < 2u) return false;
  if (!batch.verts[0].has_uv || !batch.verts[1].has_uv) return false;
  return batch.verts[0].x <= batch.verts[1].x && batch.verts[0].y <= batch.verts[1].y;
}

// The A-Spec display buffer: the CT16 field the HUD writes into (FBP 0 or 140). A panel draw into another FRAME is an
// off-screen render target (the tyre widget's page 499), composited later by a draw the captured panel does not carry.
bool display_buffer_target(const HudDrawState &state) { return state.frame_psm() == 2u && (state.frame_fbp() == 0u ||
                                                                                           state.frame_fbp() == 140u); }

bool panel_path1_matches_speed(const TapState &state, const GifPacket &packet) {
  if (!state.frame_known || ((state.frame >> 24) & 0x3Fu) != 2u) return false;  // the display buffer is CT16
  const std::uint32_t fbp = static_cast<std::uint32_t>(state.frame & 0x1FFu);
  if (fbp != 0u && fbp != 140u) return false;
  if (state.tex0 == 0u) return false;
  const std::uint32_t tbp0 = static_cast<std::uint32_t>(state.tex0 & 0x3FFFu);
  const std::uint32_t psm = static_cast<std::uint32_t>((state.tex0 >> 20) & 0x3Fu);
  if (tbp0 < kHudFirstTbp0 || tbp0 > kHudLastTbp0 || psm != 0x13u) return false;
  if (packet.vertices.size() < 4u) return false;
  const std::uint32_t maxu = static_cast<std::uint32_t>((state.clamp >> 14) & 0x3FFu);
  const std::uint32_t maxv = static_cast<std::uint32_t>((state.clamp >> 34) & 0x3FFu);
  if (maxu != 81u || maxv != 25u) return false;  // the measured `remote_speed_kmph` region
  return true;
}

}  // namespace

bool parse_hud_element(const std::string &name, HudElement &element) {
  if (name == "speed") {
    element = HudElement::speed;
    return true;
  }
  if (name == "tach") {
    element = HudElement::tach;
    return true;
  }
  if (name == "all") {
    element = HudElement::all;
    return true;
  }
  return false;
}

const char *hud_element_name(HudElement element) noexcept {
  switch (element) {
    case HudElement::speed: return "speed";
    case HudElement::tach: return "tach";
    case HudElement::all: return "all";
  }
  return "unknown";
}

const char *hud_texture_status_name(HudTextureStatus status) noexcept {
  switch (status) {
    case HudTextureStatus::ok: return "ok";
    case HudTextureStatus::refused_format: return "refused_format";
    case HudTextureStatus::refused_state: return "refused_state";
    case HudTextureStatus::refused_decode: return "refused_decode";
  }
  return "unknown";
}

// ---- TASK-214/TASK-080: aspect-aware anchoring ----

const char *hud_anchor_name(HudAnchor anchor) noexcept {
  switch (anchor) {
    case HudAnchor::centre: return "centre";
    case HudAnchor::left: return "left";
    case HudAnchor::right: return "right";
  }
  return "unknown";
}

double HudAspectLayout::anchor_offset(HudAnchor anchor) const noexcept {
  if (!anchored) return 0.0;
  const double added = virtual_width - reference_width;
  switch (anchor) {
    case HudAnchor::left: return 0.0;
    case HudAnchor::centre: return added * 0.5;
    case HudAnchor::right: return added;
  }
  return 0.0;
}

HudAspectLayout hud_aspect_layout(double canvas_width, double canvas_height, std::uint32_t output_width,
                                  std::uint32_t output_height, double target_aspect, double reference_aspect) {
  HudAspectLayout layout;
  layout.reference_width = canvas_width;
  layout.reference_aspect = reference_aspect;
  layout.target_aspect = target_aspect;
  layout.anchored = std::isfinite(target_aspect) && target_aspect > 0.0 && std::isfinite(reference_aspect) &&
                    reference_aspect > 0.0;
  layout.virtual_width = layout.anchored ? canvas_width * (target_aspect / reference_aspect) : canvas_width;
  if (layout.virtual_width <= 0.0) layout.virtual_width = canvas_width;
  layout.scale_x = static_cast<double>(output_width) / layout.virtual_width;
  layout.scale_y = static_cast<double>(output_height) / canvas_height;
  return layout;
}

HudAnchor hud_classify_anchor(std::uint32_t tbp0, float min_x, float max_x, double canvas_width,
                              float edge_margin) {
  // The measured A-Spec (scene 7600) page zones, in the game's own wide 16:9 layout: the position/lap block (pages
  // 0x3C20/0x3C40/0x3C60/0x3C80 at x 21.8..129) and the minimap (0x3B60/0x3B80/0x3C00 at x 25.5..160.5) are the left
  // block. The race-font page 0x3000 carries the centre lap timer (x 297.5..364) and FULL THROTTLE (x 260.5..378.2)
  // as well as the right-hand Best/Last Lap times (x 531.5..596); the measured split is x 500.
  switch (tbp0) {
    case 0x3B60u:
    case 0x3B80u:
    case 0x3C00u:
    case 0x3C20u:
    case 0x3C40u:
    case 0x3C60u:
    case 0x3C80u: return HudAnchor::left;
    default: break;
  }
  if (tbp0 == 0x3000u) return min_x >= 500.0f ? HudAnchor::right : HudAnchor::centre;
  // The geometric fallback (an element on a page the census did not classify): the game's own wideOffset distance is
  // the boundary between the edge block and the central cluster.
  const double left_margin = static_cast<double>(min_x);
  const double right_margin = canvas_width - static_cast<double>(max_x);
  if (left_margin <= static_cast<double>(edge_margin) && left_margin <= right_margin) return HudAnchor::left;
  if (right_margin <= static_cast<double>(edge_margin) && right_margin < left_margin) return HudAnchor::right;
  return HudAnchor::centre;
}

bool build_hud_stream_vram(const std::vector<GsDecodeImageUpload> &uploads, std::uint64_t stream_vblank,
                           bool include_previous, SlmImage &slm, HudUploadCounters &counters, std::string &error,
                           const SlmImage *base) {
  slm = SlmImage{};
  if (base != nullptr) {
    slm.local = base->local;
    slm.covered = base->covered;
    counters.reasons["base image supplied (the sidecar's frame record)"] += 0;
  }
  error.clear();
  for (const GsDecodeImageUpload &upload : uploads) {
    if (upload.vblank > stream_vblank) continue;
    if (!include_previous && upload.vblank != stream_vblank) continue;
    counters.uploads += 1;
    if (upload.trxdir != 0u) {
      counters.refused += 1;
      counters.reasons["IMAGE transfer is not host->local"] += 1;
      continue;
    }
    if (!upload.has_transfer_state) {
      counters.refused += 1;
      counters.reasons["IMAGE without BITBLTBUF/TRXREG"] += 1;
      continue;
    }
    if (upload.dpsm != 0u && upload.dpsm != 0x13u && upload.dpsm != 0x14u) {
      counters.refused += 1;
      counters.reasons["upload format outside CT32/T8/T4"] += 1;
      continue;
    }
    if (upload.rrw == 0u || upload.rrh == 0u) {
      counters.refused += 1;
      counters.reasons["upload with an empty TRXREG rectangle"] += 1;
      continue;
    }
    const auto psm = static_cast<hwgs::memory::Psm>(upload.dpsm);
    const std::size_t expected = hwgs::memory::hostBytesFor(psm, upload.rrw, upload.rrh);
    if (upload.data.size() < expected) {
      counters.refused += 1;
      counters.reasons["upload payload shorter than its TRXREG rectangle"] += 1;
      continue;
    }
    const hwgs::memory::BufferView destination{upload.dbp, upload.dbw, psm};
    const hwgs::memory::TransferRect rect{upload.dsax, upload.dsay, upload.rrw, upload.rrh};
    hwgs::memory::hostToLocal(slm.local, destination, rect, std::span<const std::uint8_t>(upload.data.data(), expected));
    // Cover the destination pages the upload wrote, so the texture decoder can tell a captured block from the zero fill.
    const hwgs::memory::PsmInfo &info = hwgs::memory::psmInfo(psm);
    const std::uint32_t per_row = std::max<std::uint32_t>(1u, upload.dbw * 64u / info.pageWidth);
    const std::uint32_t last_x = (upload.dsax + upload.rrw - 1u) & 0x7FFu;
    const std::uint32_t last_y = (upload.dsay + upload.rrh - 1u) & 0x7FFu;
    // A T4/T8 upload carries real texel indices; the page is then covered as captured texel content (the A-Spec HUD
    // uploads its digit atlases inside the window, unlike the night B-Spec font page).
    const bool texel_upload = upload.dpsm == 0x13u || upload.dpsm == 0x14u;
    for (std::uint32_t page_y = 0; page_y <= last_y / info.pageHeight; ++page_y)
      for (std::uint32_t page_x = upload.dsax / info.pageWidth; page_x <= last_x / info.pageWidth; ++page_x) {
        slm.cover(vram::Window{upload.dbp + 32u * (page_y * per_row + page_x), 32u});
        if (texel_upload) slm.cover_texels(vram::Window{upload.dbp + 32u * (page_y * per_row + page_x), 32u});
      }
    counters.applied += 1;
    counters.bytes += expected;
  }
  if (counters.applied == 0) {
    error = "the selected stream carries no host->local upload";
    return false;
  }
  return true;
}

// ---- The font atlas page ----

namespace {

// The measured GT4meter atlas: T4, TBW 2, TW 7 (128 texels), TH 6 (64 rows), CLAMP 0..127 x 0..38, page 32 blocks.
constexpr std::uint32_t kFontAtlasMagic = 0x46345447u;  // "GT4F" in file order
constexpr std::uint32_t kFontAtlasVersion = 1u;
constexpr std::uint32_t kFontAtlasPageBytes = 8192u;
constexpr std::uint32_t kFontAtlasPageBlocks = 32u;
constexpr std::size_t kFontAtlasHeaderBytes = 40u;
constexpr std::uint32_t kFontAtlasTbw = 2u;
constexpr std::uint32_t kFontAtlasTw = 7u;
constexpr std::uint32_t kFontAtlasTh = 6u;
// The CSM1 T4 load class: a 16-entry bank is one 64-byte group at CBP.
constexpr std::size_t kFontAtlasBankBytes = 64u;

}  // namespace

bool parse_hud_font_atlas(const std::vector<std::uint8_t> &bytes, HudFontAtlas &atlas, std::string &error) {
  atlas = HudFontAtlas{};
  error.clear();
  if (bytes.size() < kFontAtlasHeaderBytes) {
    error = "font atlas is shorter than its 40-byte header";
    return false;
  }
  if (vram::read_u32(bytes.data(), 0) != kFontAtlasMagic) {
    error = "not a GT4F font atlas (bad magic)";
    return false;
  }
  const std::uint32_t version = vram::read_u32(bytes.data(), 4);
  if (version != kFontAtlasVersion) {
    error = "unsupported GT4F version " + std::to_string(version);
    return false;
  }
  atlas.tbp0 = vram::read_u32(bytes.data(), 8);
  atlas.tbw = vram::read_u32(bytes.data(), 12);
  atlas.tw = vram::read_u32(bytes.data(), 16);
  atlas.th = vram::read_u32(bytes.data(), 20);
  atlas.clamp = vram::read_u64(bytes.data(), 24);
  const std::uint32_t page_bytes = vram::read_u32(bytes.data(), 32);
  const std::uint32_t reserved = vram::read_u32(bytes.data(), 36);
  if (reserved != 0u) {
    error = "GT4F reserved word is not zero";
    return false;
  }
  if (atlas.tbw != kFontAtlasTbw || atlas.tw != kFontAtlasTw || atlas.th != kFontAtlasTh) {
    error = "font atlas geometry is not the measured T4 128x64 (TBW 2, TW 7, TH 6)";
    return false;
  }
  if (bits(atlas.clamp, 14, 23) != 127u || bits(atlas.clamp, 34, 43) != 38u) {
    error = "font atlas CLAMP is not the measured 0..127 x 0..38 region";
    return false;
  }
  if (page_bytes != kFontAtlasPageBytes) {
    error = "font atlas page size is not 8192 bytes";
    return false;
  }
  if (bytes.size() < kFontAtlasHeaderBytes + kFontAtlasPageBytes) {
    error = "font atlas is truncated before its 8192-byte page";
    return false;
  }
  if (atlas.tbp0 > vram::kBlocksTotal - kFontAtlasPageBlocks) {
    error = "font atlas TBP0 lies outside the 4 MiB image";
    return false;
  }
  atlas.page.assign(bytes.begin() + kFontAtlasHeaderBytes,
                    bytes.begin() + kFontAtlasHeaderBytes + kFontAtlasPageBytes);
  return true;
}

bool apply_hud_font_atlas(const HudFontAtlas &atlas, SlmImage &slm, std::uint32_t target_tbp0,
                          std::uint32_t palette_cbp, std::string &error) {
  error.clear();
  if (atlas.page.size() != kFontAtlasPageBytes) {
    error = "font atlas page is not 8192 bytes";
    return false;
  }
  if (target_tbp0 >= vram::kBlocksTotal || target_tbp0 + kFontAtlasPageBlocks > vram::kBlocksTotal) {
    error = "font atlas target TBP0 lies outside the 4 MiB image";
    return false;
  }
  if (palette_cbp >= vram::kBlocksTotal) {
    error = "font atlas palette CBP lies outside the 4 MiB image";
    return false;
  }
  const std::span<std::uint8_t> memory = slm.local.bytes();
  const std::size_t page_offset = static_cast<std::size_t>(target_tbp0) * vram::kBlockBytes;
  const std::size_t bank_offset = static_cast<std::size_t>(palette_cbp) * vram::kBlockBytes;
  // The 16-entry CSM1 bank a T4 draw with CSA 0 samples is the 64 bytes at CBP; the page write would replace it with
  // the atlas's unused rows 48..63. Preserve the frame's captured bank across the page write.
  std::array<std::uint8_t, kFontAtlasBankBytes> bank{};
  std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(bank_offset), kFontAtlasBankBytes, bank.begin());
  std::copy(atlas.page.begin(), atlas.page.end(), memory.begin() + static_cast<std::ptrdiff_t>(page_offset));
  std::copy(bank.begin(), bank.end(), memory.begin() + static_cast<std::ptrdiff_t>(bank_offset));
  // The page's 32 blocks are now real content (the decoder refuses blocks outside the covered set).
  slm.cover(vram::Window{target_tbp0, kFontAtlasPageBlocks});
  slm.cover_texels(vram::Window{target_tbp0, kFontAtlasPageBlocks});
  return true;
}

const HudTexture &HudTextureCache::get(const HudDrawState &state, std::string &error) {
  const std::uint64_t key_tex0 = state.tex0 & ~(std::uint64_t{7} << 61);  // CLD is not part of the content key
  const auto key = std::make_tuple(key_tex0, state.tex1, state.clamp);
  const auto cached = cache_.find(key);
  if (cached != cache_.end()) {
    cache_hits_ += 1;
    if (cached->second.status != HudTextureStatus::ok) error = cached->second.detail;
    return cached->second;
  }
  HudTexture texture;
  texture.tex0 = key_tex0;
  texture.tex1 = state.tex1;
  texture.clamp = state.clamp;
  const std::uint32_t psm = state.tex0_psm();
  if (psm != 0x14u && psm != 0x13u) {
    char detail[96];
    std::snprintf(detail, sizeof detail, "PSM 0x%02X is not the measured T4/T8 HUD format", static_cast<unsigned>(psm));
    texture.status = HudTextureStatus::refused_format;
    texture.detail = detail;
    refusals_ += 1;
    reasons_[texture.detail] += 1;
    error = texture.detail;
    return cache_.emplace(key, std::move(texture)).first->second;
  }
  // The synthesized PGLUtexture record: the captured TEX0/TEX1/CLAMP. CLD is forced to 1 because the captured state
  // loads the palette bank through the TEX2 write that precedes the draw, and the sidecar holds the bank at CBP; the
  // record path reads exactly that bank. The four MIPTBP words are zero (MXL 0).
  std::uint8_t record[40] = {};
  // CLD is bits 61-63; the state loads the palette through the TEX2 write, so the synthesized record asks the record
  // path for that bank (CLD = 1) and the sidecar supplies its bytes.
  const std::uint64_t tex0 = (state.tex0 & ~(std::uint64_t{7} << 61)) | (std::uint64_t{1} << 61);
  std::memcpy(record + 0, &tex0, 8);
  std::memcpy(record + 8, &state.tex1, 8);
  std::memcpy(record + 32, &state.clamp, 8);
  DecodedTexture decoded;
  std::string decode_error;
  const TextureStatus status = decode_texture(slm_, record, sizeof record, TextureDecodeOptions{}, decoded,
                                              decode_error);
  if (status != TextureStatus::ok) {
    texture.status = status == TextureStatus::refused_psm ? HudTextureStatus::refused_format
                                                          : HudTextureStatus::refused_decode;
    texture.detail = texture_status_name(status) + std::string(": ") + decode_error;
    refusals_ += 1;
    reasons_[decode_error] += 1;
    error = texture.detail;
    return cache_.emplace(key, std::move(texture)).first->second;
  }
  texture.status = HudTextureStatus::ok;
  texture.detail = decoded.detail;
  texture.width = decoded.width;
  texture.height = decoded.height;
  texture.x0 = decoded.x0;
  texture.y0 = decoded.y0;
  texture.rgba = decoded.rgba;
  // A page whose texels the captured stream itself uploaded is real content: a constant RGB/alpha page is then a
  // legitimate solid texture (the A-Spec gear box, the map dot), not the missing-font-atlas placeholder. Only a page
  // the stream did not upload as T4/T8 texels keeps the S3 refusal (the GT4meter page arrives as palette transfers
  // only, so the night B-Spec digits stay refused by the measured reason).
  const bool font_page = font_atlas_pages_.count(state.tex0_tbp0()) != 0;
  const bool texel_source = slm_.covers_texels(state.tex0_tbp0());
  const std::size_t lanes = font_page ? 4u : 3u;
  bool constant = !texture.rgba.empty();
  for (std::size_t pixel = 1; constant && pixel * 4u + (lanes - 1u) < texture.rgba.size(); ++pixel)
    for (std::size_t lane = 0; lane < lanes; ++lane)
      if (texture.rgba[pixel * 4u + lane] != texture.rgba[lane]) constant = false;
  if (constant && !font_page && !texel_source) {
    texture.status = HudTextureStatus::refused_state;
    texture.detail = "texture page holds a constant value (the font atlas is uploaded before the captured window)";
    refusals_ += 1;
    reasons_[texture.detail] += 1;
    error = texture.detail;
    return cache_.emplace(key, std::move(texture)).first->second;
  }
  decodes_ += 1;
  return cache_.emplace(key, std::move(texture)).first->second;
}

bool build_hud_panel(const std::vector<GsDecodeBatch> &batches, const std::vector<std::uint8_t> &tap_data,
                     const std::vector<TapFileRecord> &tap_records, std::uint64_t scene_vblank, HudElement element,
                     HudPanel &panel, std::string &error) {
  panel = HudPanel{};
  panel.scene_vblank = scene_vblank;
  panel.stream_vblank = scene_vblank + 1u;
  panel.element = element;
  HudPanelCounters &counters = panel.counters;

  // ---- PATH2: the panel range of the attributed stream ----
  // The present blit is the measured full-screen CT16 sample of the world page; the panel sprites follow it and stop at
  // the next full-screen batch (the trailing colour clear). Every sprite in between belongs to the panel.
  std::size_t present = batches.size();
  for (std::size_t index = 0; index < batches.size(); ++index) {
    const GsDecodeBatch &batch = batches[index];
    if (batch.vblank != panel.stream_vblank) continue;
    if (!batch.full_screen || batch.path != 2) continue;
    if (!prim_tme(batch.state.prim_value()) || batch.state.frame_psm() != 2u) continue;
    if (batch.state.tex0_tbp0() != 0x1A40u) continue;
    present = index;
    break;
  }
  if (present == batches.size()) {
    counters.refusals += 1;
    counters.count("no present blit in the attributed stream");
    error = "the attributed stream has no measured present blit";
    return false;
  }

  // The merged SCISSOR timeline: the tap's writes in execution order, consumed by job id. This is the measured fix for
  // the stale PATH2-only scissor (the plate's VU1 job restores the full-screen rectangle before the digits draw).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> tap_scissors;  // (job, SCISSOR word)
  {
    TapState state;
    for (const TapFileRecord &record : tap_records) {
      if (record.kind != 1u) continue;
      if (record.offset + record.bytes > tap_data.size()) continue;
      const GifPacket packet = parse_gif(tap_data.data() + record.offset, record.bytes);
      if (!packet.ok) continue;
      for (const GifRegisterWrite &write : packet.ad_writes) {
        apply_tap_register(state, write.address, write.data);
        if ((write.address & 0x7Fu) == 0x40u || (write.address & 0x7Fu) == 0x41u)
          tap_scissors.emplace_back(record.job, write.data);
      }
    }
  }
  const auto effective_scissor = [&](std::uint64_t job, std::uint64_t stream_scissor) {
    std::uint64_t scissor = stream_scissor;
    for (const auto &entry : tap_scissors) {
      if (entry.first <= job) scissor = entry.second;
    }
    return scissor;
  };

  std::uint64_t first_job = UINT64_MAX;
  std::uint64_t last_job = 0;
  for (const GsDecodeBatch &batch : batches) {
    if (batch.vblank != panel.stream_vblank) continue;
    first_job = std::min(first_job, batch.last_job);
    last_job = std::max(last_job, batch.last_job);
  }
  if (first_job == UINT64_MAX) first_job = 0;
  // The A-Spec selection and the census start the panel at the present blit: earlier jobs are the frame's 3D work.
  // The speed element keeps its measured range (the S3 night gate is byte-identical with it).
  const std::uint64_t panel_first_job =
      element == HudElement::speed ? first_job : std::max(first_job, batches[present].last_job);
  const std::uint64_t panel_last_job = last_job;

  for (std::size_t index = present + 1; index < batches.size(); ++index) {
    const GsDecodeBatch &batch = batches[index];
    if (batch.vblank != panel.stream_vblank) break;
    if (batch.full_screen) break;  // the trailing clear ends the panel
    counters.path2_batches += 1;
    if (element == HudElement::all && !display_buffer_target(from_decode_state(batch.state))) {
      // An off-screen render target (the tyre widget's page 499): its composite is not in the captured panel range.
      counters.count("panel sprite targets an off-screen buffer (composite outside the captured panel)");
      counters.path2_other += 1;
      continue;
    }
    const bool selected = element == HudElement::all ? panel_path2_matches_all(batch)
                                                     : panel_path2_matches(batch, element);
    if (!selected) {
      counters.path2_other += 1;
      continue;
    }
    HudDraw draw;
    draw.path1 = false;
    draw.job = batch.last_job;
    draw.sequence = batch.sequence;
    draw.state = from_decode_state(batch.state);
    draw.state.scissor = effective_scissor(batch.last_job, batch.state.scissor);
    draw.vertices = std::min<std::uint32_t>(2u, static_cast<std::uint32_t>(batch.verts.size()));
    if (draw.vertices < 2u) {
      counters.refusals += 1;
      counters.count("speed sprite with fewer than two vertices");
      continue;
    }
    if (!(batch.verts[0].x <= batch.verts[1].x) || !(batch.verts[0].y <= batch.verts[1].y)) {
      // The measured sprites are top-left/bottom-right pairs; a reversed pair would need the GS's own corner selection.
      counters.refusals += 1;
      counters.count("speed sprite is not a top-left/bottom-right pair");
      continue;
    }
    for (std::uint32_t v = 0; v < draw.vertices; ++v) {
      const GsDecodeVertex &source = batch.verts[v];
      if (!source.has_uv) {
        counters.refusals += 1;
        counters.count("speed sprite without a UV");
        draw.vertices = 0;
        break;
      }
      draw.verts[v].x = source.x;
      draw.verts[v].y = source.y;
      draw.verts[v].u = source.u;
      draw.verts[v].v = source.v;
      draw.verts[v].rgba = draw.state.rgba;
      draw.verts[v].drawing = source.drawing;
    }
    if (draw.vertices == 0) continue;
    draw.min_x = std::min(draw.verts[0].x, draw.verts[1].x);
    draw.max_x = std::max(draw.verts[0].x, draw.verts[1].x);
    draw.min_y = std::min(draw.verts[0].y, draw.verts[1].y);
    draw.max_y = std::max(draw.verts[0].y, draw.verts[1].y);
    // TASK-214: the night B-Spec speed element is the bottom-right plate and digits; the census-driven `all` mode uses
    // the measured page/position rule. The central cluster (the `tach` element) is centre-anchored.
    draw.anchor = element == HudElement::speed
                      ? HudAnchor::right
                      : hud_classify_anchor(draw.state.tex0_tbp0(), draw.min_x, draw.max_x, 640.0);
    // Evidence of the merge: the PATH2-only scissor would have culled this sprite.
    const ScissorRect stream_scissor = scissor_rect(batch.state.scissor);
    if (!stream_scissor.contains(draw.min_x, draw.max_x, draw.min_y, draw.max_y) &&
        scissor_rect(draw.state.scissor).contains(draw.min_x, draw.max_x, draw.min_y, draw.max_y))
      counters.path2_stream_scissor_culled += 1;
    char line[256];
    std::snprintf(line, sizeof line,
                  "path2 seq %u job %llu sprite x %.2f..%.2f y %.2f..%.2f uv %.2f..%.2f x %.2f..%.2f tbp0 0x%04X "
                  "rgba 0x%08X",
                  draw.sequence, static_cast<unsigned long long>(draw.job), draw.min_x, draw.max_x, draw.min_y,
                  draw.max_y, draw.verts[0].u, draw.verts[1].u, draw.verts[0].v, draw.verts[1].v,
                  draw.state.tex0_tbp0(), draw.state.rgba);
    panel.summary += line;
    panel.summary += "\n";
    panel.draws.push_back(draw);
    counters.path2_selected += 1;
  }

  // ---- PATH1: the element plate draws of the tap, in the stream's job range ----
  if (tap_records.empty()) {
    counters.refusals += 1;
    counters.count("no tap: the element plate is PATH1 and cannot be drawn");
  } else {
    TapState state;
    std::uint32_t record_ordinal = 0;
    std::vector<HudDraw> candidates;  // TASK-090 A-Spec: every plate/needle candidate of the panel job range
    for (const TapFileRecord &record : tap_records) {
      if (record.kind != 1u) continue;
      ++record_ordinal;
      const bool in_panel_jobs = record.job >= panel_first_job && record.job <= panel_last_job;
      if (record.offset + record.bytes > tap_data.size()) {
        if (in_panel_jobs) {
          counters.refusals += 1;
          counters.count("tap record runs past the file");
        }
        continue;
      }
      const GifPacket packet = parse_gif(tap_data.data() + record.offset, record.bytes);
      if (!packet.ok) {
        // A malformed transfer outside the panel's VU1 jobs is other work's business; only the panel range counts.
        if (in_panel_jobs) {
          counters.refusals += 1;
          counters.count("tap packet parser refused a record");
        }
        continue;
      }
      for (const GifRegisterWrite &write : packet.ad_writes) apply_tap_register(state, write.address, write.data);
      if (packet.vertices.empty()) continue;
      if (!state.frame_known || ((state.frame >> 24) & 0x3Fu) != 2u) continue;
      const std::uint32_t fbp = static_cast<std::uint32_t>(state.frame & 0x1FFu);
      if (fbp != 0u && fbp != 140u) continue;
      counters.path1_draws += 1;
      if (record.job < panel_first_job || record.job > panel_last_job) {
        counters.path1_other += 1;
        continue;
      }
      if (element == HudElement::speed && !panel_path1_matches_speed(state, packet)) {
        counters.path1_other += 1;
        continue;
      }
      if (element == HudElement::tach || element == HudElement::all) {
        // Collect every panel draw the A-Spec selection may use: the textured axis-aligned plates and the untextured
        // triangle strips (the gauge needles). Everything else is counted by name, never guessed into a shape.
        HudDraw draw;
        draw.path1 = true;
        draw.job = record.job;
        draw.sequence = record_ordinal;
        draw.state.prim = packet.tags.empty() ? packet.vertices[0].prim
                                              : (packet.tags[0].prim != 0u ? packet.tags[0].prim
                                                                            : packet.vertices[0].prim);
        draw.state.frame = state.frame;
        draw.state.zbuf = state.zbuf;
        draw.state.test = state.test;
        draw.state.alpha = state.alpha;
        draw.state.scissor = state.scissor;
        draw.state.xyoffset = state.xyoffset;
        draw.state.tex0 = state.tex0;
        draw.state.tex1 = state.tex1;
        draw.state.clamp = state.clamp;
        const std::uint32_t offset_x = static_cast<std::uint32_t>(state.xyoffset & 0xFFFFu);
        const std::uint32_t offset_y = static_cast<std::uint32_t>((state.xyoffset >> 32) & 0xFFFFu);
        const std::uint32_t vertices = static_cast<std::uint32_t>(std::min<std::size_t>(packet.vertices.size(), 8u));
        bool any_drawing = false;
        float min_u = 1e9f, max_u = -1e9f, min_v = 1e9f, max_v = -1e9f;
        for (std::uint32_t v = 0; v < vertices; ++v) {
          const GifVertex &source = packet.vertices[v];
          draw.verts[v].x = screen_coordinate(source.x, offset_x);
          draw.verts[v].y = screen_coordinate(source.y, offset_y);
          draw.verts[v].u = source.s;
          draw.verts[v].v = source.t;
          draw.verts[v].rgba = source.r | (static_cast<std::uint32_t>(source.g) << 8) |
                               (static_cast<std::uint32_t>(source.b) << 16) |
                               (static_cast<std::uint32_t>(source.a) << 24);
          draw.verts[v].drawing = !source.adc;
          any_drawing = any_drawing || draw.verts[v].drawing;
          min_u = std::min(min_u, source.s);
          max_u = std::max(max_u, source.s);
          min_v = std::min(min_v, source.t);
          max_v = std::max(max_v, source.t);
        }
        // Non-IIP strips carry one flat colour: pin every vertex to the first drawing one.
        if (((draw.state.prim >> 3) & 1u) == 0u) {
          std::uint32_t flat = draw.verts[0].rgba;
          for (std::uint32_t v = 0; v < vertices; ++v)
            if (draw.verts[v].drawing) {
              flat = draw.verts[v].rgba;
              break;
            }
          for (std::uint32_t v = 0; v < vertices; ++v) draw.verts[v].rgba = flat;
        }
        draw.vertices = vertices;
        draw.min_x = draw.max_x = draw.verts[0].x;
        draw.min_y = draw.max_y = draw.verts[0].y;
        for (std::uint32_t v = 1; v < vertices; ++v) {
          draw.min_x = std::min(draw.min_x, draw.verts[v].x);
          draw.max_x = std::max(draw.max_x, draw.verts[v].x);
          draw.min_y = std::min(draw.min_y, draw.verts[v].y);
          draw.max_y = std::max(draw.max_y, draw.verts[v].y);
        }
        draw.anchor = element == HudElement::tach
                          ? HudAnchor::centre
                          : hud_classify_anchor(draw.state.tex0_tbp0(), draw.min_x, draw.max_x, 640.0);
        const bool textured = prim_tme(draw.state.prim) && draw.state.tex0 != 0u;
        const bool quad = vertices == 4u && (draw.verts[0].y == draw.verts[1].y) &&
                          (draw.verts[2].y == draw.verts[3].y) && (draw.verts[0].x == draw.verts[2].x) &&
                          (draw.verts[1].x == draw.verts[3].x) && max_u > min_u && max_v > min_v;
        if (textured && quad) {
          draw.normalized_uv = true;
          draw.triangle_strip = true;
          draw.state.rgba = draw.verts[0].rgba;
          candidates.push_back(draw);
        } else if (textured) {
          counters.count("plate quad is not the measured axis-aligned ST map");
          counters.path1_other += 1;
        } else if (prim_type(draw.state.prim) == 4u && packet.vertices.size() >= 3u && any_drawing) {
          if (packet.vertices.size() > 8u) {
            counters.count("needle strip above the measured 8-vertex bound");
            counters.path1_other += 1;
          } else {
            draw.untextured_strip = true;
            candidates.push_back(draw);
          }
        } else {
          counters.count("untextured PATH1 draw that is not a triangle strip");
          counters.path1_other += 1;
        }
        continue;
      }
      HudDraw draw;
      draw.path1 = true;
      draw.job = record.job;
      draw.sequence = record_ordinal;
      draw.state.prim = packet.tags.empty() ? packet.vertices[0].prim
                                            : (packet.tags[0].prim != 0u ? packet.tags[0].prim
                                                                          : packet.vertices[0].prim);
      draw.state.frame = state.frame;
      draw.state.zbuf = state.zbuf;
      draw.state.test = state.test;
      draw.state.alpha = state.alpha;
      draw.state.scissor = state.scissor;
      draw.state.xyoffset = state.xyoffset;
      draw.state.tex0 = state.tex0;
      draw.state.tex1 = state.tex1;
      draw.state.clamp = state.clamp;
      draw.normalized_uv = true;
      draw.triangle_strip = true;
      draw.vertices = 4;
      std::uint32_t offset_x = static_cast<std::uint32_t>(state.xyoffset & 0xFFFFu);
      std::uint32_t offset_y = static_cast<std::uint32_t>((state.xyoffset >> 32) & 0xFFFFu);
      float min_u = 1e9f, max_u = -1e9f, min_v = 1e9f, max_v = -1e9f;
      for (std::uint32_t v = 0; v < 4u; ++v) {
        const GifVertex &source = packet.vertices[v];
        draw.verts[v].x = screen_coordinate(source.x, offset_x);
        draw.verts[v].y = screen_coordinate(source.y, offset_y);
        draw.verts[v].u = source.s;
        draw.verts[v].v = source.t;
        draw.verts[v].rgba = source.r | (static_cast<std::uint32_t>(source.g) << 8) |
                             (static_cast<std::uint32_t>(source.b) << 16) |
                             (static_cast<std::uint32_t>(source.a) << 24);
        draw.verts[v].drawing = !source.adc;
        min_u = std::min(min_u, source.s);
        max_u = std::max(max_u, source.s);
        min_v = std::min(min_v, source.t);
        max_v = std::max(max_v, source.t);
      }
      // The measured quad is axis aligned and maps ST monotonically. Anything else is refused, never guessed.
      const bool axis_aligned = (draw.verts[0].y == draw.verts[1].y) && (draw.verts[2].y == draw.verts[3].y) &&
                                (draw.verts[0].x == draw.verts[2].x) && (draw.verts[1].x == draw.verts[3].x);
      if (!axis_aligned || max_u - min_u <= 0.0f || max_v - min_v <= 0.0f) {
        counters.refusals += 1;
        counters.count("plate quad is not the measured axis-aligned ST map");
        counters.path1_other += 1;
        continue;
      }
      draw.state.rgba = draw.verts[0].rgba;  // PRIM.FST: the flat colour of the first vertex
      draw.min_x = std::min(std::min(draw.verts[0].x, draw.verts[1].x), std::min(draw.verts[2].x, draw.verts[3].x));
      draw.max_x = std::max(std::max(draw.verts[0].x, draw.verts[1].x), std::max(draw.verts[2].x, draw.verts[3].x));
      draw.min_y = std::min(std::min(draw.verts[0].y, draw.verts[1].y), std::min(draw.verts[2].y, draw.verts[3].y));
      draw.max_y = std::max(std::max(draw.verts[0].y, draw.verts[1].y), std::max(draw.verts[2].y, draw.verts[3].y));
      // TASK-214: the night B-Spec speed plate is the measured bottom-right element.
      draw.anchor = HudAnchor::right;
      if (draw.min_x < 545.0f || draw.max_x > 620.0f || draw.min_y < 375.0f || draw.max_y > 406.0f) {
        counters.path1_other += 1;
        continue;
      }
      char line[256];
      std::snprintf(line, sizeof line,
                    "path1 job %llu plate x %.2f..%.2f y %.2f..%.2f st %.3f..%.3f x %.3f..%.3f tbp0 0x%04X",
                    static_cast<unsigned long long>(draw.job), draw.min_x, draw.max_x, draw.min_y, draw.max_y, min_u,
                    max_u, min_v, max_v, draw.state.tex0_tbp0());
      panel.summary += line;
      panel.summary += "\n";
      panel.draws.push_back(draw);
      counters.path1_selected += 1;
      counters.plate_quads += 1;
    }

    if (element == HudElement::tach) {
      // The tachometer is the right gauge: the dial plate is a 256x128 T4 page whose measured CLAMP region is 130x100,
      // drawn as an axis-aligned quad at x 348..445.5; its needle is the untextured triangle strip of the same VU1 job.
      const HudDraw *plate = nullptr;
      for (const HudDraw &candidate : candidates) {
        if (candidate.untextured_strip) continue;
        if (candidate.state.tex0_psm() != 0x14u || candidate.state.tex0_tw() != 8u || candidate.state.tex0_th() != 7u)
          continue;
        const std::uint32_t width = candidate.state.clamp_maxu() - candidate.state.clamp_minu() + 1u;
        const std::uint32_t height = candidate.state.clamp_maxv() - candidate.state.clamp_minv() + 1u;
        if (width != 130u || height != 100u) continue;
        if (candidate.min_x <= 300.0f) continue;  // the left dial is the speedometer
        plate = &candidate;
        break;
      }
      if (plate == nullptr) {
        counters.refusals += 1;
        counters.count("no tachometer dial plate (right 130x100 T4 gauge) in the panel range");
        error = "the requested tachometer element has no measured dial plate";
        return false;
      }
      panel.summary += "path1 tachometer dial plate at x " + std::to_string(plate->min_x) + ".." +
                       std::to_string(plate->max_x) + " tbp0 0x" + std::to_string(plate->state.tex0_tbp0()) + "\n";
      panel.draws.push_back(*plate);
      counters.path1_selected += 1;
      counters.plate_quads += 1;
      for (const HudDraw &candidate : candidates) {
        if (!candidate.untextured_strip || candidate.job != plate->job) continue;
        panel.draws.push_back(candidate);
        counters.path1_selected += 1;
        counters.path1_needles += 1;
        char line[256];
        std::snprintf(line, sizeof line,
                      "path1 needle job %llu x %.2f..%.2f y %.2f..%.2f vertices %u",
                      static_cast<unsigned long long>(candidate.job), candidate.min_x, candidate.max_x,
                      candidate.min_y, candidate.max_y, candidate.vertices);
        panel.summary += line;
        panel.summary += "\n";
      }
      if (counters.path1_needles == 0) {
        counters.refusals += 1;
        counters.count("tachometer dial plate has no untextured needle strip in its job");
      }
    } else if (element == HudElement::all) {
      for (const HudDraw &candidate : candidates) {
        panel.draws.push_back(candidate);
        counters.path1_selected += 1;
        if (candidate.untextured_strip) {
          counters.path1_needles += 1;
        } else {
          counters.plate_quads += 1;
        }
      }
    }
  }

  // The executed order: a VU1 job posted before a PATH2 packet runs before it, so a PATH1 draw sorts before a PATH2
  // draw with the same (or a larger) job id.
  std::stable_sort(panel.draws.begin(), panel.draws.end(), [](const HudDraw &left, const HudDraw &right) {
    if (left.job != right.job) return left.job < right.job;
    return left.path1 && !right.path1;
  });
  // TASK-214: the anchor census of the selected panel (reported and asserted by the fixture).
  for (const HudDraw &draw : panel.draws) {
    switch (draw.anchor) {
      case HudAnchor::left: counters.anchor_left += 1; break;
      case HudAnchor::centre: counters.anchor_centre += 1; break;
      case HudAnchor::right: counters.anchor_right += 1; break;
    }
  }

  if (panel.draws.empty()) {
    counters.refusals += 1;
    counters.count("no element draw selected");
    error = "no draw of the requested element was selected";
    return false;
  }
  return true;
}

bool build_hud_census(const std::vector<GsDecodeBatch> &batches, const std::vector<std::uint8_t> &tap_data,
                      const std::vector<TapFileRecord> &tap_records, std::uint64_t stream_vblank,
                      HudCensus &census, std::string &error) {
  census = HudCensus{};
  census.stream_vblank = stream_vblank;
  error.clear();

  // The measured panel range: the present blit (the full-screen CT16 sample of the world page) starts it, the next
  // full-screen batch (the trailing clear) ends it. This is the same rule build_hud_panel uses.
  std::size_t present = batches.size();
  for (std::size_t index = 0; index < batches.size(); ++index) {
    const GsDecodeBatch &batch = batches[index];
    if (batch.vblank != stream_vblank) continue;
    if (!batch.full_screen || batch.path != 2) continue;
    if (!prim_tme(batch.state.prim_value()) || batch.state.frame_psm() != 2u) continue;
    if (batch.state.tex0_tbp0() != 0x1A40u) continue;
    present = index;
    break;
  }
  if (present == batches.size()) {
    error = "the attributed stream has no measured present blit";
    return false;
  }

  std::uint64_t first_job = UINT64_MAX;
  std::uint64_t last_job = 0;
  for (const GsDecodeBatch &batch : batches) {
    if (batch.vblank != stream_vblank) continue;
    first_job = std::min(first_job, batch.last_job);
    last_job = std::max(last_job, batch.last_job);
  }
  if (first_job == UINT64_MAX) first_job = 0;
  // The panel starts at the present blit; earlier jobs are the frame's 3D work.
  const std::uint64_t panel_first_job = std::max(first_job, batches[present].last_job);
  const std::uint64_t panel_last_job = last_job;

  // ---- PATH2: every batch of the panel range ----
  for (std::size_t index = present + 1; index < batches.size(); ++index) {
    const GsDecodeBatch &batch = batches[index];
    if (batch.vblank != stream_vblank) break;
    if (batch.full_screen) break;
    census.path2_batches += 1;
    if (batch.path != 2) {
      census.reasons["panel batch is not PATH2"] += 1;
      continue;
    }
    bool drawing = false;
    for (const GsDecodeVertex &vertex : batch.verts) drawing = drawing || vertex.drawing;
    if (!drawing) {
      census.reasons["panel batch has no drawing vertex"] += 1;
      continue;
    }
    census.path2_drawing += 1;
    HudCensusEntry entry;
    entry.path1 = false;
    entry.job = batch.last_job;
    entry.sequence = batch.sequence;
    entry.state = from_decode_state(batch.state);
    entry.vertices = static_cast<std::uint32_t>(batch.verts.size());
    entry.min_x = batch.min_x;
    entry.max_x = batch.max_x;
    entry.min_y = batch.min_y;
    entry.max_y = batch.max_y;
    entry.drawing = true;
    // The measured sprite form: two vertices, top-left then bottom-right. A rotated or four-vertex sprite is kept in
    // the census with axis_aligned = false instead of being guessed into the axis-aligned raster.
    entry.axis_aligned = batch.verts.size() >= 2u && batch.verts[0].x <= batch.verts[1].x &&
                         batch.verts[0].y <= batch.verts[1].y;
    census.entries.push_back(entry);
  }

  // ---- PATH1: the tap's display-buffer draws inside the stream's job range ----
  if (tap_records.empty()) {
    census.reasons["no tap: the panel's PATH1 plates are unavailable"] += 1;
  } else {
    TapState state;
    std::uint32_t record_ordinal = 0;
    for (const TapFileRecord &record : tap_records) {
      if (record.kind != 1u) continue;
      ++record_ordinal;
      const bool in_panel_jobs = record.job >= panel_first_job && record.job <= panel_last_job;
      if (record.offset + record.bytes > tap_data.size()) {
        if (in_panel_jobs) census.reasons["tap record runs past the file"] += 1;
        continue;
      }
      const GifPacket packet = parse_gif(tap_data.data() + record.offset, record.bytes);
      if (!packet.ok) {
        if (in_panel_jobs) census.reasons["tap packet parser refused a record"] += 1;
        continue;
      }
      for (const GifRegisterWrite &write : packet.ad_writes) apply_tap_register(state, write.address, write.data);
      if (packet.vertices.empty()) continue;
      if (!state.frame_known || ((state.frame >> 24) & 0x3Fu) != 2u) continue;  // the CT16 display buffer
      const std::uint32_t fbp = static_cast<std::uint32_t>(state.frame & 0x1FFu);
      if (fbp != 0u && fbp != 140u) continue;
      census.path1_display_draws += 1;
      if (record.job < panel_first_job || record.job > panel_last_job) {
        census.reasons["PATH1 display draw outside the stream job range"] += 1;
        continue;
      }
      HudCensusEntry entry;
      entry.path1 = true;
      entry.job = record.job;
      entry.sequence = record_ordinal;
      entry.state.prim = packet.tags.empty() ? packet.vertices[0].prim
                                             : (packet.tags[0].prim != 0u ? packet.tags[0].prim
                                                                           : packet.vertices[0].prim);
      entry.state.frame = state.frame;
      entry.state.zbuf = state.zbuf;
      entry.state.test = state.test;
      entry.state.alpha = state.alpha;
      entry.state.scissor = state.scissor;
      entry.state.xyoffset = state.xyoffset;
      entry.state.tex0 = state.tex0;
      entry.state.tex1 = state.tex1;
      entry.state.clamp = state.clamp;
      entry.vertices = static_cast<std::uint32_t>(packet.vertices.size());
      const std::uint32_t offset_x = static_cast<std::uint32_t>(state.xyoffset & 0xFFFFu);
      const std::uint32_t offset_y = static_cast<std::uint32_t>((state.xyoffset >> 32) & 0xFFFFu);
      bool first = true;
      bool axis_aligned = packet.vertices.size() == 4u;
      float min_u = 1e9f, max_u = -1e9f, min_v = 1e9f, max_v = -1e9f;
      for (std::uint32_t v = 0; v < packet.vertices.size() && v < 4u; ++v) {
        const GifVertex &source = packet.vertices[v];
        const float screen_x = screen_coordinate(source.x, offset_x);
        const float screen_y = screen_coordinate(source.y, offset_y);
        if (first) {
          entry.min_x = entry.max_x = screen_x;
          entry.min_y = entry.max_y = screen_y;
          first = false;
        } else {
          entry.min_x = std::min(entry.min_x, screen_x);
          entry.max_x = std::max(entry.max_x, screen_x);
          entry.min_y = std::min(entry.min_y, screen_y);
          entry.max_y = std::max(entry.max_y, screen_y);
        }
        min_u = std::min(min_u, source.s);
        max_u = std::max(max_u, source.s);
        min_v = std::min(min_v, source.t);
        max_v = std::max(max_v, source.t);
        entry.state.rgba = source.r | (static_cast<std::uint32_t>(source.g) << 8) |
                           (static_cast<std::uint32_t>(source.b) << 16) |
                           (static_cast<std::uint32_t>(source.a) << 24);
        entry.drawing = entry.drawing || !source.adc;
      }
      // The measured plate quad: vertex 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right, ST monotonic.
      if (packet.vertices.size() != 4u) axis_aligned = false;
      if (packet.vertices.size() == 4u) {
        const float x0 = screen_coordinate(packet.vertices[0].x, offset_x);
        const float x1 = screen_coordinate(packet.vertices[1].x, offset_x);
        const float y0 = screen_coordinate(packet.vertices[0].y, offset_y);
        const float y2 = screen_coordinate(packet.vertices[2].y, offset_y);
        const bool corners = (packet.vertices[0].y == packet.vertices[1].y) &&
                             (packet.vertices[2].y == packet.vertices[3].y) &&
                             (packet.vertices[0].x == packet.vertices[2].x) &&
                             (packet.vertices[1].x == packet.vertices[3].x);
        axis_aligned = corners && x0 <= x1 && y0 <= y2 && max_u > min_u && max_v > min_v;
      }
      entry.axis_aligned = axis_aligned;
      if (axis_aligned) census.path1_axis_aligned += 1;
      census.entries.push_back(entry);
    }
  }

  // The executed order, the same rule as the panel: a PATH1 draw of a job runs before a PATH2 packet with that job id.
  std::stable_sort(census.entries.begin(), census.entries.end(), [](const HudCensusEntry &left,
                                                                  const HudCensusEntry &right) {
    if (left.job != right.job) return left.job < right.job;
    return left.path1 && !right.path1;
  });
  return true;
}

std::uint8_t ct16_expand(std::uint8_t value8) noexcept { return static_cast<std::uint8_t>(value8 & 0xF8u); }

bool draw_hud_panel(const HudPanel &panel, HudTextureCache &textures, std::vector<std::uint8_t> &rgba,
                    std::uint32_t width, std::uint32_t height, const HudDrawOptions &options,
                    std::vector<std::uint8_t> *mask, HudDrawCounters &counters, std::string &error,
                    std::vector<std::uint8_t> *contrib_mask) {
  if (width == 0 || height == 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4u) {
    error = "HUD target is smaller than its stated size";
    return false;
  }
  if (options.canvas_width == 0 || options.canvas_height == 0) {
    error = "HUD canvas is empty";
    return false;
  }
  // TASK-214: the aspect-aware canvas-to-output mapping. With no target aspect the mapping is the captured one (a target
  // pixel centre maps to `(p + 0.5) * canvas / target - 0.5`), so the S3 coverage rule below reproduces the measured
  // pixels byte for byte. A target aspect widens the HUD canvas to 480*R units and shifts each element by its measured
  // anchor; at 16:9 the shift is zero and the mapping is identical to the captured one.
  const double reference_aspect = std::isfinite(options.reference_aspect) && options.reference_aspect > 0.0
                                      ? options.reference_aspect
                                      : kHudReferenceAspect;
  if (options.target_aspect > 0.0) {
    if (!std::isfinite(options.target_aspect)) {
      error = "HUD target aspect is not a finite number";
      return false;
    }
    if (options.target_aspect < reference_aspect - 1e-6) {
      error = "HUD target aspect is narrower than the captured 16:9 layout (the game's Vert+ HUD is not captured)";
      return false;
    }
    if (options.target_aspect > 6.0) {
      error = "HUD target aspect is above the supported 6.0 maximum";
      return false;
    }
  }
  const HudAspectLayout layout = hud_aspect_layout(static_cast<double>(options.canvas_width),
                                                   static_cast<double>(options.canvas_height), width, height,
                                                   options.target_aspect, reference_aspect);
  const double scale_x = layout.scale_x;
  const double scale_y = layout.scale_y;
  // The first/last target pixel whose canvas centre lies in the half-open draw box or the inclusive scissor rectangle.
  const auto first_covered = [](double minimum, double scale) {
    return static_cast<std::int32_t>(std::ceil((minimum + 0.5) * scale - 0.5));
  };
  const auto last_covered = [](double maximum, double scale) {
    return static_cast<std::int32_t>(std::ceil((maximum + 0.5) * scale - 0.5)) - 1;
  };
  const auto last_inside_scissor = [](double maximum, double scale) {
    return static_cast<std::int32_t>(std::floor((maximum + 0.5) * scale - 0.5));
  };
  if (mask != nullptr) mask->assign(static_cast<std::size_t>(width) * height, 0);
  if (contrib_mask != nullptr) contrib_mask->assign(static_cast<std::size_t>(width) * height, 0);
  // TASK-090 HUD A-Spec: the untextured Gouraud triangle-strip raster (the gauge needles). Coverage is a pixel centre
  // inside a triangle on the GS 1/16 subpixel grid with the top-left fill rule, so the two triangles of a strip do not
  // write their shared edge twice; the vertex colours interpolate linearly and the captured ALPHA/TEST/CT16 rules apply.
  const auto composite_strip_pixel = [&](std::int32_t px, std::int32_t py, const std::array<std::uint8_t, 4> &fragment,
                                         const BlendEquation &equation, bool blend) {
    if (contrib_mask != nullptr && fragment[3] > 0)
      (*contrib_mask)[static_cast<std::size_t>(py) * width + px] = 255;
    std::uint8_t *destination = rgba.data() + (static_cast<std::size_t>(py) * width + px) * 4u;
    std::array<std::uint8_t, 4> cd = {destination[0], destination[1], destination[2], destination[3]};
    if (options.ct16) {
      cd[0] = ct16_expand(cd[0]);
      cd[1] = ct16_expand(cd[1]);
      cd[2] = ct16_expand(cd[2]);
    }
    std::array<std::uint8_t, 4> out = fragment;
    if (blend) {
      blend_fragment(equation, fragment, cd, out);
      counters.blended += 1;
    }
    if (options.ct16) {
      out[0] = ct16_store(out[0]);
      out[1] = ct16_store(out[1]);
      out[2] = ct16_store(out[2]);
    }
    out[3] = 255;
    destination[0] = out[0];
    destination[1] = out[1];
    destination[2] = out[2];
    destination[3] = out[3];
    counters.pixels += 1;
    if (mask != nullptr) (*mask)[static_cast<std::size_t>(py) * width + px] = 255;
  };
  for (const HudDraw &draw : panel.draws) {
    counters.draws += 1;
    const HudDrawState &state = draw.state;
    // TASK-214: the element's horizontal anchor translation in captured-canvas units (0 at the captured 16:9 aspect).
    const double anchor_dx = layout.anchor_offset(draw.anchor);
    if (draw.untextured_strip) {
      const BlendEquation equation = decode_blend_equation(state.alpha);
      const bool blend = prim_abe(state.prim);
      // The measured needle ALPHA is (Cs-Cd)*FIX+Cd (0x64, FIX 21/128); the sprite/plate set uses the normal family.
      const bool measured_blend = blend_family(state.alpha) == BlendFamily::normal ||
                                  (equation.a == 0u && equation.b == 1u && equation.c == 2u && equation.d == 1u);
      if (blend && !measured_blend) {
        counters.refused += 1;
        counters.reasons["ALPHA equation outside the measured normal/FIX families"] += 1;
        continue;
      }
      std::int32_t first_x = first_covered(draw.min_x + anchor_dx, scale_x);
      std::int32_t last_x = last_covered(draw.max_x + anchor_dx, scale_x);
      std::int32_t first_y = first_covered(draw.min_y, scale_y);
      std::int32_t last_y = last_covered(draw.max_y, scale_y);
      const ScissorRect scissor = scissor_rect(state.scissor);
      if (!scissor.empty()) {
        first_x = std::max(first_x, first_covered(static_cast<double>(scissor.x0) + anchor_dx, scale_x));
        last_x = std::min(last_x, last_inside_scissor(static_cast<double>(scissor.x1) + anchor_dx, scale_x));
        first_y = std::max(first_y, first_covered(static_cast<double>(scissor.y0), scale_y));
        last_y = std::min(last_y, last_inside_scissor(static_cast<double>(scissor.y1), scale_y));
      }
      first_x = std::max(first_x, 0);
      first_y = std::max(first_y, 0);
      last_x = std::min(last_x, static_cast<std::int32_t>(width) - 1);
      last_y = std::min(last_y, static_cast<std::int32_t>(height) - 1);
      if (draw.vertices < 3u || last_x < first_x || last_y < first_y) {
        counters.refused += 1;
        counters.reasons["needle strip outside the scissor or the target"] += 1;
        continue;
      }
      // The 1/16 subpixel points of the strip and the exact edge functions.
      std::array<std::array<std::int64_t, 2>, 8> points{};
      bool drawing_vertex[8] = {};
      for (std::uint32_t v = 0; v < draw.vertices; ++v) {
        points[v][0] = std::llround(static_cast<double>(draw.verts[v].x) * 16.0);
        points[v][1] = std::llround(static_cast<double>(draw.verts[v].y) * 16.0);
        drawing_vertex[v] = draw.verts[v].drawing;
      }
      const auto edge_function = [](const std::array<std::int64_t, 2> &a, const std::array<std::int64_t, 2> &b,
                                    std::int64_t px, std::int64_t py) {
        return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
      };
      const auto top_left_edge = [](const std::array<std::int64_t, 2> &a, const std::array<std::int64_t, 2> &b) {
        const std::int64_t dx = b[0] - a[0], dy = b[1] - a[1];
        return dy < 0 || (dy == 0 && dx > 0);
      };
      for (std::uint32_t i = 0; i + 2u < draw.vertices; ++i) {
        if (!drawing_vertex[i] || !drawing_vertex[i + 1] || !drawing_vertex[i + 2]) continue;
        std::uint32_t ia = i, ib = i + 1u, ic = i + 2u;
        std::int64_t area = edge_function(points[ia], points[ib], points[ic][0], points[ic][1]);
        if (area == 0) {
          counters.reasons["degenerate needle triangle"] += 1;
          continue;
        }
        if (area < 0) {
          std::swap(ib, ic);
          area = -area;
        }
        const std::array<std::int64_t, 2> a = points[ia], b = points[ib], c = points[ic];
        for (std::int32_t py = first_y; py <= last_y; ++py) {
          for (std::int32_t px = first_x; px <= last_x; ++px) {
            const double canvas_x = (static_cast<double>(px) + 0.5) / scale_x - 0.5 - anchor_dx;
            const double canvas_y = (static_cast<double>(py) + 0.5) / scale_y - 0.5;
            const std::int64_t sample_x = std::llround(canvas_x * 16.0);
            const std::int64_t sample_y = std::llround(canvas_y * 16.0);
            const std::int64_t e0 = edge_function(b, c, sample_x, sample_y);
            const std::int64_t e1 = edge_function(c, a, sample_x, sample_y);
            const std::int64_t e2 = edge_function(a, b, sample_x, sample_y);
            const bool inside = (e0 > 0 || (e0 == 0 && top_left_edge(b, c))) &&
                                (e1 > 0 || (e1 == 0 && top_left_edge(c, a))) &&
                                (e2 > 0 || (e2 == 0 && top_left_edge(a, b)));
            if (!inside) continue;
            counters.sampled += 1;
            const double w0 = static_cast<double>(e0) / static_cast<double>(area);
            const double w1 = static_cast<double>(e1) / static_cast<double>(area);
            const double w2 = static_cast<double>(e2) / static_cast<double>(area);
            const auto interpolate = [&](std::size_t lane) {
              const double value = w0 * ((draw.verts[ia].rgba >> (8u * lane)) & 0xFFu) +
                                   w1 * ((draw.verts[ib].rgba >> (8u * lane)) & 0xFFu) +
                                   w2 * ((draw.verts[ic].rgba >> (8u * lane)) & 0xFFu);
              return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            };
            const std::array<std::uint8_t, 4> fragment = {interpolate(0), interpolate(1), interpolate(2),
                                                          interpolate(3)};
            const AlphaTestResult tested = alpha_test_ate_only(state.test, fragment[3]);
            if (tested == AlphaTestResult::fail) {
              counters.alpha_test_failed += 1;
              continue;
            }
            if (tested != AlphaTestResult::pass) {
              counters.refused += 1;
              counters.reasons[std::string("alpha test: ") + alpha_test_result_name(tested)] += 1;
              continue;
            }
            composite_strip_pixel(px, py, fragment, equation, blend);
          }
        }
      }
      continue;
    }
    if (!prim_tme(state.prim)) {
      counters.refused += 1;
      counters.reasons["untextured HUD draw"] += 1;
      continue;
    }
    std::string texture_error;
    const HudTexture &texture = textures.get(state, texture_error);
    if (texture.status != HudTextureStatus::ok) {
      counters.refused += 1;
      counters.reasons[texture_error] += 1;
      continue;
    }
    // The sampler's measured subset: MODULATE, MXL 0, matching filters, region clamp.
    if (state.tex0_tfx() != 0u || state.tex1_mxl() != 0u || state.tex1_mmag() != state.tex1_mmin()) {
      counters.refused += 1;
      counters.reasons["sampler state outside the measured subset"] += 1;
      continue;
    }
    SampledTexture sampled;
    sampled.psm = state.tex0_psm();
    sampled.tw = state.tex0_tw();
    sampled.th = state.tex0_th();
    sampled.tcc = state.tex0_tcc();
    sampled.tfx = state.tex0_tfx();
    sampled.mxl = state.tex1_mxl();
    sampled.mmag = state.tex1_mmag() != 0u ? 1u : 0u;
    sampled.mmin = state.tex1_mmin() == 0u ? 0u : (state.tex1_mmin() == 1u ? 1u : 2u);
    if (sampled.mmin > 1u) {
      counters.refused += 1;
      counters.reasons["sampler minification filter outside the measured subset"] += 1;
      continue;
    }
    sampled.wms = state.clamp_wms();
    sampled.wmt = state.clamp_wmt();
    sampled.minu = state.clamp_minu();
    sampled.maxu = state.clamp_maxu();
    sampled.minv = state.clamp_minv();
    sampled.maxv = state.clamp_maxv();
    sampled.width = texture.width;
    sampled.height = texture.height;
    sampled.x0 = texture.x0;
    sampled.y0 = texture.y0;
    sampled.rgba = texture.rgba.data();

    std::int32_t first_x = first_covered(draw.min_x + anchor_dx, scale_x);
    std::int32_t last_x = last_covered(draw.max_x + anchor_dx, scale_x);
    std::int32_t first_y = first_covered(draw.min_y, scale_y);
    std::int32_t last_y = last_covered(draw.max_y, scale_y);
    const ScissorRect scissor = scissor_rect(state.scissor);
    if (!scissor.empty()) {
      first_x = std::max(first_x, first_covered(static_cast<double>(scissor.x0) + anchor_dx, scale_x));
      last_x = std::min(last_x, last_inside_scissor(static_cast<double>(scissor.x1) + anchor_dx, scale_x));
      first_y = std::max(first_y, first_covered(static_cast<double>(scissor.y0), scale_y));
      last_y = std::min(last_y, last_inside_scissor(static_cast<double>(scissor.y1), scale_y));
    }
    first_x = std::max(first_x, 0);
    first_y = std::max(first_y, 0);
    last_x = std::min(last_x, static_cast<std::int32_t>(width) - 1);
    last_y = std::min(last_y, static_cast<std::int32_t>(height) - 1);
    if (last_x < first_x || last_y < first_y) {
      counters.refused += 1;
      counters.reasons["element draw outside the scissor or the target"] += 1;
      continue;
    }
    // The draw's two measured corners: UV interpolation runs between vertex 0 and vertex 1 (sprites) or across the
    // plate quad's min/max corners (the axis-aligned strip).
    float u_x0 = draw.verts[0].x, u_x1 = draw.verts[1].x, u_u0 = draw.verts[0].u, u_u1 = draw.verts[1].u;
    float u_y0 = draw.verts[0].y, u_y1 = draw.verts[1].y, u_v0 = draw.verts[0].v, u_v1 = draw.verts[1].v;
    if (draw.path1) {
      u_x0 = draw.min_x;
      u_x1 = draw.max_x;
      u_y0 = draw.min_y;
      u_y1 = draw.max_y;
      u_u0 = draw.verts[0].u;
      u_u1 = draw.verts[1].u;
      u_v0 = draw.verts[0].v;
      u_v1 = draw.verts[2].v;
    }
    if (u_x1 == u_x0 || u_y1 == u_y0) {
      counters.refused += 1;
      counters.reasons["degenerate HUD rectangle"] += 1;
      continue;
    }
    const BlendEquation equation = decode_blend_equation(state.alpha);
    const bool blend = prim_abe(state.prim);
    if (blend && blend_family(state.alpha) == BlendFamily::other) {
      counters.refused += 1;
      counters.reasons["ALPHA equation outside the measured normal family"] += 1;
      continue;
    }
    std::array<std::uint8_t, 4> vertex = {static_cast<std::uint8_t>(state.rgba & 0xFFu),
                                          static_cast<std::uint8_t>((state.rgba >> 8) & 0xFFu),
                                          static_cast<std::uint8_t>((state.rgba >> 16) & 0xFFu),
                                          static_cast<std::uint8_t>((state.rgba >> 24) & 0xFFu)};
    if (draw.path1) {
      const std::uint32_t flat = draw.verts[0].rgba;
      vertex = {static_cast<std::uint8_t>(flat & 0xFFu), static_cast<std::uint8_t>((flat >> 8) & 0xFFu),
                static_cast<std::uint8_t>((flat >> 16) & 0xFFu), static_cast<std::uint8_t>((flat >> 24) & 0xFFu)};
    }
    for (std::int32_t py = first_y; py <= last_y; ++py) {
      for (std::int32_t px = first_x; px <= last_x; ++px) {
        // The CoreGS 2.0 coverage rule places each covered pixel at its integer centre of the 2D canvas
        // (`ceil(min) .. ceil(max) - 1`); the target pixel's canvas centre is the interpolation point, so a 3x output
        // samples at 1/3-pixel steps and the UVs and texel filters match the reference's 3x internal raster.
        const float fx = static_cast<float>((static_cast<double>(px) + 0.5) / scale_x - 0.5 - anchor_dx);
        const float fy = static_cast<float>((static_cast<double>(py) + 0.5) / scale_y - 0.5);
        float u = u_u0 + (fx - u_x0) / (u_x1 - u_x0) * (u_u1 - u_u0);
        float v = u_v0 + (fy - u_y0) / (u_y1 - u_y0) * (u_v1 - u_v0);
        if (draw.path1) {
          // The plate's UVs are normalized ST. The measured corners: vertex 0 top-left, vertex 1 top-right,
          // vertex 2 bottom-left.
          u = u_u0 + (fx - u_x0) / (u_x1 - u_x0) * (draw.verts[1].u - draw.verts[0].u);
          v = u_v0 + (fy - u_y0) / (u_y1 - u_y0) * (draw.verts[2].v - draw.verts[0].v);
        }
        // PATH2 sprites carry texel UVs (the measured digit cells are 91..105 x 1..19); the sampler takes normalized
        // ST like the GS, so a non-normalized draw is divided by its texture size. PATH1 plates already carry ST.
        float sample_u = u;
        float sample_v = v;
        if (!draw.normalized_uv) {
          sample_u = u / static_cast<float>(1u << sampled.tw);
          sample_v = v / static_cast<float>(1u << sampled.th);
        }
        std::array<std::uint8_t, 4> fragment{};
        if (sample_texture(sampled, sample_u, sample_v, vertex, fragment) != SampleStatus::ok) {
          counters.refused += 1;
          counters.reasons["texture sample refused"] += 1;
          continue;
        }
        counters.sampled += 1;
        const AlphaTestResult tested = alpha_test_ate_only(state.test, fragment[3]);
        if (tested == AlphaTestResult::fail) {
          counters.alpha_test_failed += 1;
          continue;
        }
        if (tested != AlphaTestResult::pass) {
          counters.refused += 1;
          counters.reasons[std::string("alpha test: ") + alpha_test_result_name(tested)] += 1;
          continue;
        }
        if (contrib_mask != nullptr && fragment[3] > 0)
          (*contrib_mask)[static_cast<std::size_t>(py) * width + px] = 255;
        std::uint8_t *destination = rgba.data() + (static_cast<std::size_t>(py) * width + px) * 4u;
        std::array<std::uint8_t, 4> cd = {destination[0], destination[1], destination[2], destination[3]};
        if (options.ct16) {
          cd[0] = ct16_expand(cd[0]);
          cd[1] = ct16_expand(cd[1]);
          cd[2] = ct16_expand(cd[2]);
        }
        std::array<std::uint8_t, 4> out = fragment;
        if (blend) {
          blend_fragment(equation, fragment, cd, out);
          counters.blended += 1;
        }
        if (options.ct16) {
          out[0] = ct16_store(out[0]);
          out[1] = ct16_store(out[1]);
          out[2] = ct16_store(out[2]);
        }
        out[3] = 255;
        destination[0] = out[0];
        destination[1] = out[1];
        destination[2] = out[2];
        destination[3] = out[3];
        counters.pixels += 1;
        if (mask != nullptr) (*mask)[static_cast<std::size_t>(py) * width + px] = 255;
      }
    }
  }
  if (mask != nullptr) {
    for (std::uint8_t value : *mask) counters.masked_pixels += value != 0 ? 1u : 0u;
  }
  return true;
}

std::string hud_anchor_report(const HudPanel &panel, const HudAspectLayout &layout) {
  std::string text =
      "index,path,sequence,job,anchor,tbp0,min_x,max_x,min_y,max_y,anchor_dx,out_x0,out_x1,out_y0,out_y1\n";
  char line[512];
  std::uint32_t index = 0;
  for (const HudDraw &draw : panel.draws) {
    const double dx = layout.anchor_offset(draw.anchor);
    std::snprintf(line, sizeof line, "%u,%s,%u,%llu,%s,0x%04X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  index, draw.path1 ? "path1" : "path2", draw.sequence,
                  static_cast<unsigned long long>(draw.job), hud_anchor_name(draw.anchor), draw.state.tex0_tbp0(),
                  draw.min_x, draw.max_x, draw.min_y, draw.max_y, dx,
                  (static_cast<double>(draw.min_x) + dx) * layout.scale_x,
                  (static_cast<double>(draw.max_x) + dx) * layout.scale_x,
                  static_cast<double>(draw.min_y) * layout.scale_y,
                  static_cast<double>(draw.max_y) * layout.scale_y);
    text += line;
    index += 1;
  }
  return text;
}

std::string hud_panel_report(const HudPanel &panel, const HudDrawCounters &draws, const HudTextureCache &textures) {
  char line[1024];
  std::string text;
  std::snprintf(line, sizeof line,
                "HUD panel: element %s, scene frame %llu (stream %llu), draws %zu (PATH2 %llu, PATH1 %llu)\n",
                hud_element_name(panel.element), static_cast<unsigned long long>(panel.scene_vblank),
                static_cast<unsigned long long>(panel.stream_vblank), panel.draws.size(),
                static_cast<unsigned long long>(panel.counters.path2_selected),
                static_cast<unsigned long long>(panel.counters.path1_selected));
  text += line;
  std::snprintf(line, sizeof line,
                "HUD selection: path2 batches %llu selected %llu other %llu stream-scissor-culled %llu; "
                "path1 draws %llu selected %llu other %llu needles %llu; refusals %llu\n",
                static_cast<unsigned long long>(panel.counters.path2_batches),
                static_cast<unsigned long long>(panel.counters.path2_selected),
                static_cast<unsigned long long>(panel.counters.path2_other),
                static_cast<unsigned long long>(panel.counters.path2_stream_scissor_culled),
                static_cast<unsigned long long>(panel.counters.path1_draws),
                static_cast<unsigned long long>(panel.counters.path1_selected),
                static_cast<unsigned long long>(panel.counters.path1_other),
                static_cast<unsigned long long>(panel.counters.path1_needles),
                static_cast<unsigned long long>(panel.counters.refusals));
  text += line;
  std::snprintf(line, sizeof line, "HUD anchors: left %llu centre %llu right %llu (TASK-214 element rule)\n",
                static_cast<unsigned long long>(panel.counters.anchor_left),
                static_cast<unsigned long long>(panel.counters.anchor_centre),
                static_cast<unsigned long long>(panel.counters.anchor_right));
  text += line;
  std::snprintf(line, sizeof line,
                "HUD textures: decodes %llu cache hits %llu refusals %llu; raster draws %llu pixels %llu masked %llu "
                "sampled %llu blended %llu alpha-fail %llu refused %llu\n",
                static_cast<unsigned long long>(textures.decodes()),
                static_cast<unsigned long long>(textures.cache_hits()),
                static_cast<unsigned long long>(textures.refusals()),
                static_cast<unsigned long long>(draws.draws), static_cast<unsigned long long>(draws.pixels),
                static_cast<unsigned long long>(draws.masked_pixels),
                static_cast<unsigned long long>(draws.sampled), static_cast<unsigned long long>(draws.blended),
                static_cast<unsigned long long>(draws.alpha_test_failed),
                static_cast<unsigned long long>(draws.refused));
  text += line;
  for (const auto &entry : panel.counters.reasons) {
    std::snprintf(line, sizeof line, "HUD selection refusal: %s x %llu\n", entry.first.c_str(),
                  static_cast<unsigned long long>(entry.second));
    text += line;
  }
  for (const auto &entry : draws.reasons) {
    std::snprintf(line, sizeof line, "HUD raster refusal: %s x %llu\n", entry.first.c_str(),
                  static_cast<unsigned long long>(entry.second));
    text += line;
  }
  for (const auto &entry : textures.reasons()) {
    std::snprintf(line, sizeof line, "HUD texture refusal: %s x %llu\n", entry.first.c_str(),
                  static_cast<unsigned long long>(entry.second));
    text += line;
  }
  if (!panel.summary.empty()) {
    text += panel.summary;
  }
  return text;
}

}  // namespace gt4::coregs3
