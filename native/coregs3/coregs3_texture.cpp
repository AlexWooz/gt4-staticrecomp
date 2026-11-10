// TASK-090 CoreGS 3.0: the native decoder of the car-pass textures. See coregs3_texture.hpp for the contract and the
// reused MIT pieces. Every refusal here is explicit: the decoder never fills an unsupported case with a guess.
#include "coregs3_texture.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

#include "coregs3_vram.hpp"
#include "hw_clut.hpp"
#include "hw_gs_registers.hpp"
#include "hw_gs_replace/names.hpp"

namespace gt4::coregs3 {
namespace {

// The sidecar reader's bounds: a diagnostic of a few captured frames, never an unbounded input.
constexpr std::size_t kMaxVramRecords = 4096;
constexpr std::size_t kMaxVramFileBytes = 512u << 20;

bool read_file(const std::string &path, std::vector<std::uint8_t> &data, std::string &error) {
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "cannot open " + path;
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    error = "cannot seek " + path;
    return false;
  }
  const long size = std::ftell(file);
  if (size < 0 || static_cast<std::size_t>(size) > kMaxVramFileBytes) {
    std::fclose(file);
    error = "VRAM sidecar is larger than the 512 MiB bound";
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    error = "cannot rewind " + path;
    return false;
  }
  data.resize(static_cast<std::size_t>(size));
  const bool ok = data.empty() || std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  if (!ok) error = "cannot read " + path;
  return ok;
}

// Texels of the GS local memory in the replace subsystem's terms. This is the source the CoreGS 2.0 texture dump uses
// (core2_renderer.cpp, SlmTexels): ReadPixel goes through the same swizzle model.
class SlmTexels final : public hw_gs_replace::TexelSource {
 public:
  explicit SlmTexels(const hwgs::memory::LocalMemory &local) : local_(local) {}

  void ReadBlock(std::uint32_t blockNumber, std::span<std::uint8_t, 256> out) const override {
    const std::size_t offset = static_cast<std::size_t>(blockNumber & (vram::kBlocksTotal - 1u)) * vram::kBlockBytes;
    std::memcpy(out.data(), local_.bytes().data() + offset, vram::kBlockBytes);
  }

  std::uint32_t ReadPixel(hw_gs_replace::Psm psm, std::uint32_t basePointer, std::uint32_t bufferWidth,
                          std::uint32_t x, std::uint32_t y) const override {
    const hwgs::memory::BufferView view{basePointer, bufferWidth, static_cast<hwgs::memory::Psm>(psm)};
    return local_.readPixel(view, x, y);
  }

  void ReadRow(hw_gs_replace::Psm psm, std::uint32_t basePointer, std::uint32_t bufferWidth, std::uint32_t x,
               std::uint32_t y, std::uint32_t count, std::uint32_t *out) const override {
    if (x + count <= 2048u && y < 2048u) {
      const hwgs::memory::BufferView view{basePointer, bufferWidth, static_cast<hwgs::memory::Psm>(psm)};
      local_.readRect(view, x, y, count, 1, out);
    } else {
      hw_gs_replace::TexelSource::ReadRow(psm, basePointer, bufferWidth, x, y, count, out);
    }
  }

 private:
  const hwgs::memory::LocalMemory &local_;
};

hw_gs_replace::WrapMode wrap_mode(std::uint32_t mode) {
  // CLAMP.WMS/WMT codes (chapter 01.1) equal hw_gs_replace's WrapMode enumeration.
  return static_cast<hw_gs_replace::WrapMode>(mode & 3u);
}

bool is_indexed(std::uint32_t psm) {
  return psm == static_cast<std::uint32_t>(hw_gs_replace::Psm::T4) ||
         psm == static_cast<std::uint32_t>(hw_gs_replace::Psm::T8);
}

// True when every block of the rectangle the decoder reads is in the sidecar. The rectangle is walked page by page;
// each GS page is 32 blocks, so the used blocks are the page's 32 entries after the page number (chapter 01.3, the same
// formula hwgs::memory::pixelSlot applies).
bool texture_blocks_covered(const SlmImage &slm, const TextureRecord &record, std::uint32_t x0, std::uint32_t y0,
                            std::uint32_t width, std::uint32_t height, std::string &error) {
  const hw_gs_replace::PsmGeometry geometry = hw_gs_replace::GeometryOf(hw_gs_replace::PsmFromCode(record.psm));
  const std::uint32_t pagesPerRow = std::max<std::uint32_t>(1u, record.tbw * 64u / geometry.pageWidth);
  const std::uint32_t xLast = x0 + width - 1u;
  const std::uint32_t yLast = y0 + height - 1u;
  for (std::uint32_t pageY = y0 / geometry.pageHeight; pageY <= yLast / geometry.pageHeight; ++pageY) {
    for (std::uint32_t pageX = x0 / geometry.pageWidth; pageX <= xLast / geometry.pageWidth; ++pageX) {
      const std::uint32_t page = pageY * pagesPerRow + pageX;
      for (std::uint32_t entry = 0; entry < 32u; ++entry) {
        if (!slm.covers(record.tbp0 + 32u * page + entry)) {
          error = "sidecar does not cover texture block " + std::to_string(record.tbp0 + 32u * page + entry);
          return false;
        }
      }
    }
  }
  return true;
}

// Builds the CSM1 32-bit palette the draw samples and decodes it with hw_gs's DecodeHalfwords. The measured car pass
// shows that TEX0.CSA selects a 16-entry group of the 16x16 palette bank at sampling time, whatever the texture's index
// width (draw 2, CSA 0, matches the CoreGS 2.0 dump exactly; draw 3, CSA 6, only matches with the bank's group 6). The
// full bank is therefore loaded unshifted (the T8 layout with CSA 0) and DecodeHalfwords applies the record's CSA.
// hw_clut's ClutUnit applies CSA at load time instead, which cancels out for an 8-bit index and makes a 4-bit index
// read the bank's first group; that model is not what the reference shows for this game.
bool decode_palette(const SlmImage &slm, const TextureRecord &record, std::uint16_t halfwords[hwgs::clut::kClutHalfwords],
                    hwgs::clut::Palette &palette, std::string &error) {
  // CSM1 32-bit: a 16x16 image at CBP, four blocks (chapter 07.4, 8-bit entry positions).
  for (std::uint32_t entry = 0; entry < 256u; ++entry) {
    std::uint32_t x = 0, y = 0;
    hwgs::clut::CsmEntryPosition(entry, true, x, y);
    hwgs::clut::PixelLocation location;
    if (!hwgs::clut::LocatePixel(record.cpsm, record.cbp, 1, x, y, location)) {
      error = "CSM1 palette pixel could not be located";
      return false;
    }
    if (!slm.covers(location.block)) {
      error = "sidecar does not cover palette block " + std::to_string(location.block);
      return false;
    }
  }

  hwgs::CounterSet counters;
  hwgs::clut::ClutUnit unit(counters);
  hwgs::clut::Tex0Write write;
  // The T8 profile makes PeekLoad copy all 256 entries; CSA 0 keeps the bank unshifted so DecodeHalfwords can select
  // the record's group. CPSM is already checked to be CT32 and CSM to be CSM1 by the caller.
  write.psm = static_cast<std::uint32_t>(hw_gs_replace::Psm::T8);
  write.cbp = record.cbp;
  write.cpsm = record.cpsm;
  write.csm = record.csm;
  write.csa = 0;
  write.cld = record.cld;
  unit.PeekLoad(write, slm.local.bytes().data(), halfwords);

  // TEXA is not part of the 40-byte PGLUtexture record. The measured car records use CT32 palettes, for which TEXA is
  // irrelevant (the 32-bit entry keeps its own alpha), so zero is not a guess here.
  palette = hwgs::clut::DecodeHalfwords(halfwords, record.psm, record.cpsm, record.csa, 0);
  return true;
}

TextureStatus refuse(DecodedTexture &out, std::string &error, TextureStatus status, std::string detail) {
  out.status = status;
  out.detail = std::move(detail);
  error = out.detail;
  return status;
}

}  // namespace

bool parse_texture_record(const std::uint8_t *bytes, std::size_t size, TextureRecord &record, std::string &error) {
  if (bytes == nullptr || size < 0x28u) {
    error = "texture record is shorter than 0x28 bytes";
    return false;
  }
  record = TextureRecord{};
  record.tex0 = vram::read_u64(bytes, 0);
  record.tex1 = vram::read_u64(bytes, 8);
  record.miptbp1 = vram::read_u64(bytes, 16);
  record.miptbp2 = vram::read_u64(bytes, 24);
  record.clamp = vram::read_u64(bytes, 32);
  record.tbp0 = hwgs::tex0::Tbp0(record.tex0);
  record.tbw = hwgs::tex0::Tbw(record.tex0);
  record.psm = hwgs::tex0::Psm(record.tex0);
  record.tw = hwgs::tex0::Tw(record.tex0);
  record.th = hwgs::tex0::Th(record.tex0);
  record.tcc = hwgs::tex0::Tcc(record.tex0);
  record.tfx = hwgs::tex0::Tfx(record.tex0);
  record.cbp = hwgs::tex0::Cbp(record.tex0);
  record.cpsm = hwgs::tex0::Cpsm(record.tex0);
  record.csm = hwgs::tex0::Csm(record.tex0);
  record.csa = hwgs::tex0::Csa(record.tex0);
  record.cld = hwgs::tex0::Cld(record.tex0);
  record.lcm = hwgs::tex1::Lcm(record.tex1);
  record.mxl = hwgs::tex1::Mxl(record.tex1);
  record.mmag = hwgs::tex1::Mmag(record.tex1);
  record.mmin = hwgs::tex1::Mmin(record.tex1);
  record.wms = hwgs::clampreg::Wms(record.clamp);
  record.wmt = hwgs::clampreg::Wmt(record.clamp);
  record.minu = hwgs::clampreg::Minu(record.clamp);
  record.maxu = hwgs::clampreg::Maxu(record.clamp);
  record.minv = hwgs::clampreg::Minv(record.clamp);
  record.maxv = hwgs::clampreg::Maxv(record.clamp);
  return true;
}

bool SlmImage::covers(std::uint32_t block) const {
  // The GS block address wraps at 16 KiB; a record range never does (parse/build refuse a wrapping range).
  block &= vram::kBlocksTotal - 1u;
  return block >= first_block && block - first_block < block_count;
}

bool parse_vram_sidecar(const std::uint8_t *data, std::size_t size, std::vector<VramRecord> &records,
                        std::string &error) {
  records.clear();
  if (data == nullptr && size != 0) {
    error = "null VRAM sidecar";
    return false;
  }
  std::size_t at = 0;
  while (at < size) {
    vram::Header header;
    if (!vram::decode_header(data + at, size - at, header, error)) return false;
    if (records.size() >= kMaxVramRecords) {
      error = "VRAM sidecar has more than the record bound";
      return false;
    }
    VramRecord record;
    record.vblank = header.vblank;
    record.first_block = header.first_block;
    record.block_count = header.block_count;
    record.bytes.assign(data + at + vram::kHeaderBytes,
                        data + at + vram::kHeaderBytes + header.block_count * vram::kBlockBytes);
    records.push_back(std::move(record));
    at += vram::kHeaderBytes + static_cast<std::size_t>(header.block_count) * vram::kBlockBytes;
  }
  if (records.empty()) {
    error = "VRAM sidecar holds no record";
    return false;
  }
  return true;
}

bool read_vram_sidecar(const std::string &path, std::vector<VramRecord> &records, std::string &error) {
  std::vector<std::uint8_t> data;
  if (!read_file(path, data, error)) return false;
  return parse_vram_sidecar(data.data(), data.size(), records, error);
}

bool build_slm_image(const VramRecord &record, SlmImage &image, std::string &error) {
  if (record.block_count == 0 || record.first_block >= vram::kBlocksTotal ||
      record.block_count > vram::kBlocksTotal - record.first_block) {
    error = "VRAM record block range is outside the 4 MiB image";
    return false;
  }
  if (record.bytes.size() != static_cast<std::size_t>(record.block_count) * vram::kBlockBytes) {
    error = "VRAM record bytes do not match its block range";
    return false;
  }
  image.first_block = record.first_block;
  image.block_count = record.block_count;
  std::fill(image.local.bytes().begin(), image.local.bytes().end(), 0u);
  std::memcpy(image.local.bytes().data() + static_cast<std::size_t>(record.first_block) * vram::kBlockBytes,
              record.bytes.data(), record.bytes.size());
  return true;
}

const char *texture_status_name(TextureStatus status) noexcept {
  switch (status) {
    case TextureStatus::ok: return "ok";
    case TextureStatus::refused_record: return "refused_record";
    case TextureStatus::refused_psm: return "refused_psm";
    case TextureStatus::refused_size: return "refused_size";
    case TextureStatus::refused_state: return "refused_state";
    case TextureStatus::refused_truncated: return "refused_truncated";
    case TextureStatus::refused_palette: return "refused_palette";
  }
  return "unknown";
}

TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             DecodedTexture &out, std::string &error) {
  return decode_texture(slm, record_bytes, size, TextureDecodeOptions{}, out, error);
}

TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             const TextureDecodeOptions &options, DecodedTexture &out, std::string &error) {
  out = DecodedTexture{};
  if (!parse_texture_record(record_bytes, size, out.record, error)) {
    return refuse(out, error, TextureStatus::refused_record, error);
  }
  const TextureRecord &record = out.record;

  // The car pass measured 163 T4, 5 T8 and one CT32 record; nothing else is decoded here.
  if (record.psm != static_cast<std::uint32_t>(hw_gs_replace::Psm::T4) &&
      record.psm != static_cast<std::uint32_t>(hw_gs_replace::Psm::T8) &&
      record.psm != static_cast<std::uint32_t>(hw_gs_replace::Psm::Ct32)) {
    char detail[96];
    std::snprintf(detail, sizeof detail, "PSM 0x%02X is not one of the measured T4/T8/CT32 formats",
                  static_cast<unsigned>(record.psm));
    return refuse(out, error, TextureStatus::refused_psm, detail);
  }
  if (record.tw > 10u || record.th > 10u) {
    char detail[96];
    std::snprintf(detail, sizeof detail, "TW %u / TH %u above the decoder's log2 bound of 10",
                  static_cast<unsigned>(record.tw), static_cast<unsigned>(record.th));
    return refuse(out, error, TextureStatus::refused_size, detail);
  }
  if (record.mxl != 0u) {
    if (!options.level0_mip_fallback) {
      char detail[96];
      std::snprintf(detail, sizeof detail, "TEX1.MXL %u: mip levels are not decoded",
                    static_cast<unsigned>(record.mxl));
      return refuse(out, error, TextureStatus::refused_state, detail);
    }
    // The world pass's declared fallback: decode level 0 (the base rectangle) and record that the record asked for mips.
    out.level0_mip_fallback = true;
  }

  const bool indexed = is_indexed(record.psm);
  if (indexed) {
    // All 169 measured records: CT32 palette, CSM1, CLD = 1, CSA 0/2/4/6. The 40-byte record carries no TEXA and no
    // TEXCLUT, so a palette state outside this set is refused instead of guessed.
    if (record.cpsm != static_cast<std::uint32_t>(hw_gs_replace::Psm::Ct32)) {
      char detail[96];
      std::snprintf(detail, sizeof detail, "palette CPSM 0x%02X is not the measured CT32",
                    static_cast<unsigned>(record.cpsm));
      return refuse(out, error, TextureStatus::refused_state, detail);
    }
    if (record.csm != 0u) {
      return refuse(out, error, TextureStatus::refused_state, "TEX0.CSM is not CSM1");
    }
    if (record.cld != 1u) {
      char detail[96];
      std::snprintf(detail, sizeof detail, "TEX0.CLD %u does not load the palette from CBP for this record",
                    static_cast<unsigned>(record.cld));
      return refuse(out, error, TextureStatus::refused_state, detail);
    }
    if (record.csa > 15u) {
      // hw_gs's two CT32 CSM1 readers disagree above 15 (one clamps, one masks); the measured set never does.
      char detail[96];
      std::snprintf(detail, sizeof detail, "CSA %u above 15 is outside the measured CSM1 palette set",
                    static_cast<unsigned>(record.csa));
      return refuse(out, error, TextureStatus::refused_state, detail);
    }
  }

  // The decoded rectangle is the CLAMP region when the register defines one, else the full texture: the same rule the
  // CoreGS 2.0 texture dump applies (core2_renderer.cpp, DumpTexture).
  hw_gs_replace::TextureDescription desc;
  desc.psm = hw_gs_replace::PsmFromCode(record.psm);
  desc.texa = hw_gs_replace::TexaState{};
  desc.wrapU = hw_gs_replace::WrapAxis{wrap_mode(record.wms), record.minu, record.maxu};
  desc.wrapV = hw_gs_replace::WrapAxis{wrap_mode(record.wmt), record.minv, record.maxv};
  desc.tw = record.tw;
  desc.th = record.th;
  desc.tbp = record.tbp0;
  desc.tbw = record.tbw;
  const hw_gs_replace::Region regionU = hw_gs_replace::ResolveRegion(desc.wrapU, desc.tw);
  const hw_gs_replace::Region regionV = hw_gs_replace::ResolveRegion(desc.wrapV, desc.th);
  out.x0 = regionU.present ? regionU.minimum : 0u;
  out.y0 = regionV.present ? regionV.minimum : 0u;
  out.width = regionU.present ? regionU.Size() : (1u << record.tw);
  out.height = regionV.present ? regionV.Size() : (1u << record.th);

  std::string coverage_error;
  if (!texture_blocks_covered(slm, record, out.x0, out.y0, out.width, out.height, coverage_error)) {
    return refuse(out, error, TextureStatus::refused_truncated, coverage_error);
  }

  // Palette (indexed formats only).
  std::array<std::uint16_t, hwgs::clut::kClutHalfwords> halfwords{};
  hw_gs_replace::ClutBuffer clut;
  hwgs::clut::Palette decoded_palette;
  std::vector<std::uint32_t> palette;
  if (indexed) {
    std::string palette_error;
    if (!decode_palette(slm, record, halfwords.data(), decoded_palette, palette_error)) {
      return refuse(out, error, TextureStatus::refused_palette, palette_error);
    }
    palette.assign(decoded_palette.entries, decoded_palette.entries + decoded_palette.count);
    out.palette_entries = decoded_palette.count;
    std::memcpy(clut.halfwords.data(), halfwords.data(), sizeof(clut.halfwords));
    desc.clut = &clut;
    desc.clutPsm = hw_gs_replace::Psm::Ct32;
    desc.csa = record.csa;
  }

  // Content key over the same rectangle, using the shared Format G key code.
  const SlmTexels source(slm.local);
  out.key = hw_gs_replace::ComputeFormatGKey(source, desc);
  out.format_p = hw_gs_replace::ComputeFormatPKeys(source, desc);
  out.format_p_name = hw_gs_replace::FormatPDumpName(out.format_p.fullChain, hw_gs_replace::ImageExtension::Png);
  if (out.key.width != out.width || out.key.height != out.height) {
    return refuse(out, error, TextureStatus::refused_state, "Format G key rectangle does not match the decode rectangle");
  }

  // RGBA8 decode: the CoreGS 2.0 dump's loop over the same source, palette and direct expansion.
  out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t stored = source.ReadPixel(desc.psm, record.tbp0, record.tbw, out.x0 + x, out.y0 + y);
      const std::uint32_t word =
          !palette.empty() ? palette[stored & (static_cast<std::uint32_t>(palette.size()) - 1u)]
                           : hw_gs_replace::ExpandDirectTexel(desc.psm, stored, desc.texa);
      std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4u, &word, 4u);
    }
  }

  char detail[128];
  std::snprintf(detail, sizeof detail, "%ux%u ok, key %016llx/%016llx", static_cast<unsigned>(out.width),
                static_cast<unsigned>(out.height), static_cast<unsigned long long>(out.key.textureHash),
                static_cast<unsigned long long>(out.key.paletteHash));
  out.status = TextureStatus::ok;
  out.detail = detail;
  error.clear();
  return TextureStatus::ok;
}

}  // namespace gt4::coregs3
