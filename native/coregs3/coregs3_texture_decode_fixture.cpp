// CTest native_coregs3_texture_decode: TASK-090 car-texture decoder on hand-authored GS memory.
//
// No game data: a synthetic 4 MiB local memory image, synthetic 40-byte PGLUtexture records and palettes written at
// hand-documented CSM1 positions. The fixture pins:
//   * record parsing (TEX0/TEX1/CLAMP bit fields) and the T4/T8/CT32 decodes;
//   * the CSM1 palette at CSA 0 and 6 (T4) and the CSA-clamp path (T8 CSA 2);
//   * the Format G content key properties: a payload byte breaks it, a palette byte breaks it, moving TBP0 with the
//     same bytes keeps it;
//   * every refusal: a short record, an unmeasured PSM, TW/TH above 10, mipmaps, CLD != 1, CSM2, a non-CT32 palette,
//     CSA above 15 and a sidecar that does not cover the texture or palette blocks;
//   * the `.gt4vram` sidecar header round trip and its malformed variants.
#include "coregs3_texture.hpp"
#include "coregs3_vram.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

void put_u32_at(std::uint8_t *out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

// The record's fields, packed exactly as TEX0/TEX1/CLAMP (chapter 01.1).
struct RecordFields {
  std::uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0, tcc = 1, tfx = 0;
  std::uint32_t cbp = 0, cpsm = 0, csm = 0, csa = 0, cld = 1;
  std::uint32_t lcm = 0, mxl = 0, mmag = 1, mmin = 1;
  std::uint32_t wms = 2, wmt = 2, minu = 0, maxu = 0, minv = 0, maxv = 0;
};

std::array<std::uint8_t, 0x28> make_record(const RecordFields &fields) {
  std::array<std::uint8_t, 0x28> bytes{};
  const std::uint64_t tex0 =
      static_cast<std::uint64_t>(fields.tbp0) | (static_cast<std::uint64_t>(fields.tbw) << 14) |
      (static_cast<std::uint64_t>(fields.psm) << 20) | (static_cast<std::uint64_t>(fields.tw) << 26) |
      (static_cast<std::uint64_t>(fields.th) << 30) | (static_cast<std::uint64_t>(fields.tcc) << 34) |
      (static_cast<std::uint64_t>(fields.tfx) << 35) | (static_cast<std::uint64_t>(fields.cbp) << 37) |
      (static_cast<std::uint64_t>(fields.cpsm) << 51) | (static_cast<std::uint64_t>(fields.csm) << 55) |
      (static_cast<std::uint64_t>(fields.csa) << 56) | (static_cast<std::uint64_t>(fields.cld) << 61);
  const std::uint64_t tex1 = static_cast<std::uint64_t>(fields.lcm) | (static_cast<std::uint64_t>(fields.mxl) << 2) |
                             (static_cast<std::uint64_t>(fields.mmag) << 5) |
                             (static_cast<std::uint64_t>(fields.mmin) << 6);
  const std::uint64_t clamp = static_cast<std::uint64_t>(fields.wms) |
                              (static_cast<std::uint64_t>(fields.wmt) << 2) |
                              (static_cast<std::uint64_t>(fields.minu) << 4) |
                              (static_cast<std::uint64_t>(fields.maxu) << 14) |
                              (static_cast<std::uint64_t>(fields.minv) << 24) |
                              (static_cast<std::uint64_t>(fields.maxv) << 34);
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((tex0 >> (8 * index)) & 0xFFu);
    bytes[8 + index] = static_cast<std::uint8_t>((tex1 >> (8 * index)) & 0xFFu);
    bytes[32 + index] = static_cast<std::uint8_t>((clamp >> (8 * index)) & 0xFFu);
  }
  return bytes;
}

SlmImage full_slm() {
  SlmImage image;
  image.first_block = 0;
  image.block_count = vram::kBlocksTotal;
  std::fill(image.local.bytes().begin(), image.local.bytes().end(), 0u);
  return image;
}

// The CT32 block tables of chapter 01.3, written out here so the fixture places palettes without the code it tests.
constexpr std::uint8_t kWordInBlock32[2][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15},
};
constexpr std::uint8_t kBlocksCt32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
};

// The CSM1 entry position (chapter 07.4): 8x2 for a 16-entry image, 16x16 for the 256-entry bank.
void csm1_position(std::uint32_t entry, bool eightBit, std::uint32_t &x, std::uint32_t &y) {
  if (eightBit) {
    x = (entry & 7u) + 8u * ((entry >> 4) & 1u);
    y = 2u * (entry >> 5) + ((entry >> 3) & 1u);
  } else {
    x = entry & 7u;
    y = entry >> 3;
  }
}

// Writes CT32 palette colors into the 16x16 CSM1 bank at CBP (page-aligned, page 0), starting at bank entry
// `first_entry`. The bank layout is written out here so the fixture does not place palettes with the code it tests.
void write_csm1_bank(hwgs::memory::LocalMemory &local, std::uint32_t cbp, std::uint32_t first_entry,
                     const std::vector<std::uint32_t> &colors) {
  require(first_entry + colors.size() <= 256u, "palette bank range");
  for (std::uint32_t index = 0; index < colors.size(); ++index) {
    std::uint32_t x = 0, y = 0;
    csm1_position(first_entry + index, true, x, y);
    const std::uint32_t block = cbp + kBlocksCt32[(y % 32u) / 8u][(x % 64u) / 8u];
    const std::uint32_t byte = 64u * ((y % 8u) / 2u) + 4u * kWordInBlock32[y & 1u][x & 7u];
    put_u32_at(local.bytes().data() + block * vram::kBlockBytes + byte, colors[index]);
  }
}

// The bank entry a logical T8 index reads for a CSM1 palette at `csa`: group (index >> 4) + CSA, clamped to the last
// group (hw_clut's DecodeHalfwords), then the entry inside the group.
std::uint32_t t8_bank_entry(std::uint32_t logical, std::uint32_t csa) {
  const std::uint32_t group = std::min<std::uint32_t>((logical >> 4) + csa, 15u);
  return 16u * group + (logical & 15u);
}

std::uint32_t palette_color(std::uint32_t location) {
  return 0xFF000000u | ((location * 3u) & 0xFFu) << 16 | ((location * 5u) & 0xFFu) << 8 | (location * 7u & 0xFFu);
}

void seed_t4(hwgs::memory::LocalMemory &local, const RecordFields &fields,
             const std::vector<std::uint32_t> &palette, std::uint32_t first_entry = 0) {
  write_csm1_bank(local, fields.cbp, first_entry, palette);
  const hwgs::memory::BufferView view{fields.tbp0, fields.tbw, hwgs::memory::Psm::T4};
  for (std::uint32_t y = 0; y < (1u << fields.th); ++y)
    for (std::uint32_t x = 0; x < (1u << fields.tw); ++x)
      local.writePixel(view, x, y, (x + 2u * y) & 15u);
}

void check_t4_region(DecodedTexture &decoded, const std::vector<std::uint32_t> &palette, const char *label) {
  require(decoded.status == TextureStatus::ok, label);
  require(decoded.width == 32 && decoded.height == 16, "T4 decoded size");
  require(decoded.palette_entries == 16, "T4 palette entries");
  require(decoded.rgba.size() == 32u * 16u * 4u, "T4 rgba size");
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t index = (x + 2u * y) & 15u;
      const std::uint32_t expected = palette[index];
      const std::uint8_t *pixel = decoded.rgba.data() + (static_cast<std::size_t>(y) * 32u + x) * 4u;
      std::uint32_t actual = 0;
      std::memcpy(&actual, pixel, 4);
      require(actual == expected, label);
    }
  }
}

void check_t4_csa0() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x2C00;
  fields.tbw = 2;
  fields.psm = 0x14;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 0;
  fields.maxu = 31;
  fields.maxv = 15;
  std::vector<std::uint32_t> palette(16);
  for (std::uint32_t entry = 0; entry < 16; ++entry) palette[entry] = palette_color(entry);
  seed_t4(slm.local, fields, palette);
  const auto record = make_record(fields);
  DecodedTexture decoded;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), decoded, error) == TextureStatus::ok, "T4 CSA 0 decodes");
  check_t4_region(decoded, palette, "T4 CSA 0 pixels");
  require(decoded.record.tbp0 == 0x2C00 && decoded.record.tw == 5 && decoded.record.th == 4, "T4 record fields");
  require(decoded.key.width == 32 && decoded.key.height == 16, "T4 key rectangle");
  require(decoded.key.textureHash != 0 && decoded.key.paletteHash != 0, "T4 key hashes");
}

void check_t4_csa6() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x2E00;
  fields.tbw = 2;
  fields.psm = 0x14;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 6;  // measured car value: the palette group at 6
  fields.maxu = 31;
  fields.maxv = 15;
  std::vector<std::uint32_t> palette(16);
  for (std::uint32_t entry = 0; entry < 16; ++entry) palette[entry] = palette_color(entry + 0x40);
  seed_t4(slm.local, fields, palette, 96);  // CSM1 bank group 6
  const auto record = make_record(fields);
  DecodedTexture decoded;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), decoded, error) == TextureStatus::ok, "T4 CSA 6 decodes");
  check_t4_region(decoded, palette, "T4 CSA 6 pixels");
}

void check_t8_csa2() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x3000;
  fields.tbw = 2;
  fields.psm = 0x13;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 2;
  fields.maxu = 31;
  fields.maxv = 15;
  std::vector<std::uint32_t> memory_colors(256);
  for (std::uint32_t location = 0; location < 256; ++location) memory_colors[location] = palette_color(location);
  write_csm1_bank(slm.local, fields.cbp, 0, memory_colors);
  const hwgs::memory::BufferView view{fields.tbp0, fields.tbw, hwgs::memory::Psm::T8};
  for (std::uint32_t y = 0; y < 16; ++y)
    for (std::uint32_t x = 0; x < 32; ++x)
      slm.local.writePixel(view, x, y, (3u * x + 5u * y) & 0xFFu);

  const auto record = make_record(fields);
  DecodedTexture decoded;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), decoded, error) == TextureStatus::ok, "T8 CSA 2 decodes");
  require(decoded.palette_entries == 256, "T8 palette entries");
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t index = (3u * x + 5u * y) & 0xFFu;
      const std::uint32_t expected = palette_color(t8_bank_entry(index, fields.csa));
      std::uint32_t actual = 0;
      std::memcpy(&actual, decoded.rgba.data() + (static_cast<std::size_t>(y) * 32u + x) * 4u, 4);
      require(actual == expected, "T8 CSA 2 pixel");
    }
  }
}

void check_ct32_direct() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x3400;
  fields.tbw = 4;
  fields.psm = 0x00;
  fields.tw = 4;
  fields.th = 4;
  fields.tcc = 1;
  fields.cld = 0;  // irrelevant for a direct format
  fields.maxu = 15;
  fields.maxv = 15;
  std::vector<std::uint32_t> words(16u * 16u);
  for (std::uint32_t y = 0; y < 16; ++y)
    for (std::uint32_t x = 0; x < 16; ++x)
      words[y * 16u + x] = 0x80000000u | (y << 8) | (x << 16) | ((x + y) & 0xFFu);
  slm.local.writeRect(hwgs::memory::BufferView{fields.tbp0, fields.tbw, hwgs::memory::Psm::Ct32}, 0, 0, 16, 16,
                      words.data());
  const auto record = make_record(fields);
  DecodedTexture decoded;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), decoded, error) == TextureStatus::ok, "CT32 decodes");
  require(decoded.palette_entries == 0 && decoded.key.paletteHash == 0, "CT32 direct palette");
  require(decoded.rgba.size() == 16u * 16u * 4u, "CT32 rgba size");
  for (std::size_t index = 0; index < words.size(); ++index) {
    std::uint32_t actual = 0;
    std::memcpy(&actual, decoded.rgba.data() + index * 4u, 4);
    require(actual == words[index], "CT32 pixel");
  }
}

void check_key_properties() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x2C00;
  fields.tbw = 2;
  fields.psm = 0x14;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 0;
  fields.maxu = 31;
  fields.maxv = 15;
  std::vector<std::uint32_t> palette(16);
  for (std::uint32_t entry = 0; entry < 16; ++entry) palette[entry] = palette_color(entry);
  seed_t4(slm.local, fields, palette);
  auto record = make_record(fields);
  DecodedTexture base;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), base, error) == TextureStatus::ok, "key base decodes");

  // A payload byte change breaks the payload hash (and only that).
  slm.local.writePixel(hwgs::memory::BufferView{fields.tbp0, fields.tbw, hwgs::memory::Psm::T4}, 0, 0, 7);
  DecodedTexture payload;
  require(decode_texture(slm, record.data(), record.size(), payload, error) == TextureStatus::ok, "payload decodes");
  require(payload.key.textureHash != base.key.textureHash, "payload change breaks the payload hash");
  require(payload.key.paletteHash == base.key.paletteHash, "payload change keeps the palette hash");
  slm.local.writePixel(hwgs::memory::BufferView{fields.tbp0, fields.tbw, hwgs::memory::Psm::T4}, 0, 0, 0);

  // A palette byte change breaks the palette hash (and only that).
  put_u32_at(slm.local.bytes().data() + fields.cbp * vram::kBlockBytes, palette_color(0) ^ 0x00000100u);
  DecodedTexture palette_changed;
  require(decode_texture(slm, record.data(), record.size(), palette_changed, error) == TextureStatus::ok,
          "palette change decodes");
  require(palette_changed.key.paletteHash != base.key.paletteHash, "palette change breaks the palette hash");
  require(palette_changed.key.textureHash == base.key.textureHash, "palette change keeps the payload hash");
  put_u32_at(slm.local.bytes().data() + fields.cbp * vram::kBlockBytes, palette_color(0));

  // Moving TBP0 with the same bytes keeps the key: copy the texture's page (32 blocks) to another base.
  const std::uint32_t moved = 0x3600;
  std::memcpy(slm.local.bytes().data() + moved * vram::kBlockBytes,
              slm.local.bytes().data() + fields.tbp0 * vram::kBlockBytes, 32u * vram::kBlockBytes);
  RecordFields moved_fields = fields;
  moved_fields.tbp0 = moved;
  const auto moved_record = make_record(moved_fields);
  DecodedTexture moved_result;
  require(decode_texture(slm, moved_record.data(), moved_record.size(), moved_result, error) == TextureStatus::ok,
          "moved TBP0 decodes");
  require(moved_result.key.textureHash == base.key.textureHash, "moving TBP0 keeps the payload hash");
  require(moved_result.key.paletteHash == base.key.paletteHash, "moving TBP0 keeps the palette hash");
  require(moved_result.key.width == base.key.width && moved_result.key.height == base.key.height,
          "moving TBP0 keeps the key rectangle");
  require(moved_result.rgba == base.rgba, "moving TBP0 keeps the image");
}

void check_refusals() {
  SlmImage slm = full_slm();
  RecordFields fields;
  fields.tbp0 = 0x2C00;
  fields.tbw = 2;
  fields.psm = 0x14;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 0;
  fields.maxu = 31;
  fields.maxv = 15;
  std::vector<std::uint32_t> palette(16);
  for (std::uint32_t entry = 0; entry < 16; ++entry) palette[entry] = palette_color(entry);
  seed_t4(slm.local, fields, palette);

  DecodedTexture decoded;
  std::string error;
  auto expect = [&](const RecordFields &bad, TextureStatus status, const char *label) {
    const auto record = make_record(bad);
    DecodedTexture result;
    std::string reason;
    const TextureStatus got = decode_texture(slm, record.data(), record.size(), result, reason);
    require(got == status, label);
    require(result.status == status, label);
    require(!result.detail.empty(), label);
  };
  RecordFields bad = fields;
  bad.psm = 0x02;  // CT16
  expect(bad, TextureStatus::refused_psm, "CT16 refused");
  bad = fields;
  bad.tw = 11;
  expect(bad, TextureStatus::refused_size, "TW 11 refused");
  bad = fields;
  bad.th = 11;
  expect(bad, TextureStatus::refused_size, "TH 11 refused");
  bad = fields;
  bad.mxl = 1;
  expect(bad, TextureStatus::refused_state, "mipmaps refused");
  bad = fields;
  bad.cld = 0;
  expect(bad, TextureStatus::refused_state, "CLD 0 refused");
  bad = fields;
  bad.csm = 1;
  expect(bad, TextureStatus::refused_state, "CSM2 refused");
  bad = fields;
  bad.cpsm = 0x02;
  expect(bad, TextureStatus::refused_state, "CT16 palette refused");
  bad = fields;
  bad.csa = 16;
  expect(bad, TextureStatus::refused_state, "CSA 16 refused");

  const auto record = make_record(fields);
  require(decode_texture(slm, record.data(), 0x27u, decoded, error) == TextureStatus::refused_record,
          "short record refused");

  // A sidecar range that does not cover the texture blocks is refused, not decoded from the zero fill.
  SlmImage cut;
  cut.first_block = 0x4000;
  cut.block_count = 32;
  std::fill(cut.local.bytes().begin(), cut.local.bytes().end(), 0u);
  const auto valid = make_record(fields);
  require(decode_texture(cut, valid.data(), valid.size(), decoded, error) == TextureStatus::refused_truncated,
          "uncovered texture refused");

  // ... and one that covers the palette but not the texture (the palette check runs after the texture check first).
  SlmImage palette_only;
  palette_only.first_block = 0x2D00;
  palette_only.block_count = 4;
  std::fill(palette_only.local.bytes().begin(), palette_only.local.bytes().end(), 0u);
  std::memcpy(palette_only.local.bytes().data() + 0x2D00 * vram::kBlockBytes,
              slm.local.bytes().data() + 0x2D00 * vram::kBlockBytes, 4u * vram::kBlockBytes);
  require(decode_texture(palette_only, valid.data(), valid.size(), decoded, error) == TextureStatus::refused_truncated,
          "uncovered texture with a covered palette refused");
}

void check_sidecar() {
  std::vector<std::uint8_t> file;
  const std::uint32_t first = vram::kCarFirstBlock;
  const std::uint32_t count = vram::kCarBlockCount;
  std::vector<std::uint8_t> blocks(static_cast<std::size_t>(count) * vram::kBlockBytes);
  for (std::size_t index = 0; index < blocks.size(); ++index) blocks[index] = static_cast<std::uint8_t>(index * 31u);
  for (std::uint64_t vblank : {7399ull, 7400ull}) {
    vram::Header header;
    header.vblank = vblank;
    header.first_block = first;
    header.block_count = count;
    std::uint8_t encoded[vram::kHeaderBytes];
    vram::encode_header(encoded, header);
    file.insert(file.end(), encoded, encoded + vram::kHeaderBytes);
    file.insert(file.end(), blocks.begin(), blocks.end());
  }
  std::vector<VramRecord> records;
  std::string error;
  require(parse_vram_sidecar(file.data(), file.size(), records, error), "sidecar parses");
  require(records.size() == 2, "sidecar record count");
  require(records[0].vblank == 7399 && records[1].vblank == 7400, "sidecar vblanks");
  require(records[0].first_block == first && records[0].block_count == count, "sidecar block range");
  require(records[0].bytes == blocks, "sidecar bytes");

  SlmImage image;
  require(build_slm_image(records[1], image, error), "sidecar image builds");
  require(image.covers(first) && image.covers(first + count - 1), "sidecar covers its range");
  require(!image.covers(first - 1) && !image.covers(first + count), "sidecar refuses outside its range");
  require(std::memcmp(image.local.bytes().data() + static_cast<std::size_t>(first) * vram::kBlockBytes,
                      blocks.data(), 1024) == 0,
          "sidecar bytes land at their block");
  require(image.local.bytes()[0] == 0, "sidecar zero fill before its range");

  auto expect_bad = [](std::vector<std::uint8_t> bytes, const char *label) {
    std::vector<VramRecord> parsed;
    std::string reason;
    require(!parse_vram_sidecar(bytes.data(), bytes.size(), parsed, reason), label);
    require(!reason.empty(), label);
  };
  std::vector<std::uint8_t> bad = file;
  bad[0] ^= 0xFFu;
  expect_bad(bad, "bad magic refused");
  bad = file;
  bad[4] = 2;
  expect_bad(bad, "unknown version refused");
  bad = file;
  bad[28] = 1;
  expect_bad(bad, "nonzero reserved refused");
  bad = file;
  bad.resize(file.size() - 1);
  expect_bad(bad, "truncated bytes refused");
  bad = file;
  bad.resize(16);
  expect_bad(bad, "truncated header refused");
}

}  // namespace

int main() {
  try {
    check_t4_csa0();
    check_t4_csa6();
    check_t8_csa2();
    check_ct32_direct();
    check_key_properties();
    check_refusals();
    check_sidecar();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 texture decode fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 texture decode fixture: ok\n");
  return 0;
}
