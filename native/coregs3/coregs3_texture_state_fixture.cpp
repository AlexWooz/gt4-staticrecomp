// CTest native_coregs3_texture_state: TASK-090 track textures' decoder-from-state path on hand-authored GS memory.
//
// No game data: a synthetic 4 MiB local memory image, a synthetic CSM1 palette and T4/T8 texels written at the
// documented chapter 01.3 / 07.4 positions. The fixture has no record table at all: every case is the external-cursor
// situation the world stream measures, where only the tapped state names the texture. It pins:
//   * the record path and the state path agree byte for byte when the state carries the same TEX0 (CLD = 1);
//   * the measured world stream shape: TEX0 with CLD = 0 plus a TEX2 palette load (CLD = 1, CSA = 0) gives the same
//     image, and the TEX2 load class rule (T4 for a CSA-0 T4 texture, T8 for any other sampled group);
//   * the CSA group selection at sampling time (the tapped TEX0's CSA, not the TEX2 load's);
//   * TEX1 (mip/filter) and CLAMP (region rectangle) are taken from the state;
//   * every refusal is explicit: a missing TEX0/TEX1/CLAMP, CLD = 0 without TEX2, a TEX2 with a different
//     CBP/CPSM/CSM/CSA/PSM, a T4 load class with a non-zero sampling CSA, and the record path's old refusals.
#include "coregs3_texture.hpp"
#include "coregs3_vram.hpp"
#include "hw_gs_registers.hpp"

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

void put_u32_at(std::uint8_t *out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
}

// The register fields, packed exactly as TEX0/TEX1/CLAMP (chapter 01.1).
struct Fields {
  std::uint32_t tbp0 = 0, tbw = 2, psm = 0x14, tw = 5, th = 4, tcc = 1, tfx = 0;
  std::uint32_t cbp = 0, cpsm = 0, csm = 0, csa = 0, cld = 1;
  std::uint32_t lcm = 0, mxl = 0, mmag = 1, mmin = 1;
  std::uint32_t wms = 2, wmt = 2, minu = 0, maxu = 0, minv = 0, maxv = 0;
};

std::uint64_t tex0_word(const Fields &fields) {
  return static_cast<std::uint64_t>(fields.tbp0) | (static_cast<std::uint64_t>(fields.tbw) << 14) |
         (static_cast<std::uint64_t>(fields.psm) << 20) | (static_cast<std::uint64_t>(fields.tw) << 26) |
         (static_cast<std::uint64_t>(fields.th) << 30) | (static_cast<std::uint64_t>(fields.tcc) << 34) |
         (static_cast<std::uint64_t>(fields.tfx) << 35) | (static_cast<std::uint64_t>(fields.cbp) << 37) |
         (static_cast<std::uint64_t>(fields.cpsm) << 51) | (static_cast<std::uint64_t>(fields.csm) << 55) |
         (static_cast<std::uint64_t>(fields.csa) << 56) | (static_cast<std::uint64_t>(fields.cld) << 61);
}

std::uint64_t tex1_word(const Fields &fields) {
  return static_cast<std::uint64_t>(fields.lcm) | (static_cast<std::uint64_t>(fields.mxl) << 2) |
         (static_cast<std::uint64_t>(fields.mmag) << 5) | (static_cast<std::uint64_t>(fields.mmin) << 6);
}

std::uint64_t clamp_word(const Fields &fields) {
  return static_cast<std::uint64_t>(fields.wms) | (static_cast<std::uint64_t>(fields.wmt) << 2) |
         (static_cast<std::uint64_t>(fields.minu) << 4) | (static_cast<std::uint64_t>(fields.maxu) << 14) |
         (static_cast<std::uint64_t>(fields.minv) << 24) | (static_cast<std::uint64_t>(fields.maxv) << 34);
}

std::array<std::uint8_t, 0x28> make_record(const Fields &fields) {
  std::array<std::uint8_t, 0x28> bytes{};
  const std::uint64_t tex0 = tex0_word(fields);
  const std::uint64_t tex1 = tex1_word(fields);
  const std::uint64_t clamp = clamp_word(fields);
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((tex0 >> (8 * index)) & 0xFFu);
    bytes[8 + index] = static_cast<std::uint8_t>((tex1 >> (8 * index)) & 0xFFu);
    bytes[32 + index] = static_cast<std::uint8_t>((clamp >> (8 * index)) & 0xFFu);
  }
  return bytes;
}

// The measured world stream's TEX2 load descriptor: the texture's TEX0 with CLD = 1, CSA = 0 and the load class PSM
// (the VU1 picks T4 for a CSA-0 texture and T8 otherwise).
std::uint64_t tex2_load_word(const Fields &fields) {
  Fields load = fields;
  load.csa = 0;
  load.cld = 1;
  load.psm = fields.csa == 0u ? 0x14u : 0x13u;
  return tex0_word(load);
}

TextureState make_state(const Fields &fields) {
  TextureState state;
  // The measured world stream: the TEX0 write that describes the texture has CLD = 0; the palette bank is uploaded by
  // the TEX2 write. The fixture's TEX2 builder below produces the matching load descriptor.
  state.tex0 = tex0_word(fields) & ~(7ull << 61);
  state.tex1 = tex1_word(fields);
  state.clamp = clamp_word(fields);
  state.tex2 = tex2_load_word(fields);
  state.tex0_known = true;
  state.tex1_known = true;
  state.clamp_known = true;
  state.tex2_known = true;
  return state;
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

void csm1_position(std::uint32_t entry, bool eight_bit, std::uint32_t &x, std::uint32_t &y) {
  if (eight_bit) {
    x = (entry & 7u) + 8u * ((entry >> 4) & 1u);
    y = 2u * (entry >> 5) + ((entry >> 3) & 1u);
  } else {
    x = entry & 7u;
    y = entry >> 3;
  }
}

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

// A palette color whose bytes make each group and entry distinguishable: R = entry, G = group, B = 0x80.
std::uint32_t palette_color(std::uint32_t group, std::uint32_t entry) {
  return 0x80000000u | (0x80u << 16) | ((group * 16u + entry) << 8) | (group * 16u + entry);
}

void seed_palette(hwgs::memory::LocalMemory &local, std::uint32_t cbp) {
  std::vector<std::uint32_t> colors(256);
  for (std::uint32_t group = 0; group < 16; ++group)
    for (std::uint32_t entry = 0; entry < 16; ++entry) colors[group * 16 + entry] = palette_color(group, entry);
  write_csm1_bank(local, cbp, 0, colors);
}

void seed_t4(hwgs::memory::LocalMemory &local, const Fields &fields) {
  const hwgs::memory::BufferView view{fields.tbp0, fields.tbw, hwgs::memory::Psm::T4};
  for (std::uint32_t y = 0; y < (1u << fields.th); ++y)
    for (std::uint32_t x = 0; x < (1u << fields.tw); ++x) local.writePixel(view, x, y, (x + 2u * y) & 15u);
}

void seed_t8(hwgs::memory::LocalMemory &slm, const Fields &fields) {
  const hwgs::memory::BufferView view{fields.tbp0, fields.tbw, hwgs::memory::Psm::T8};
  for (std::uint32_t y = 0; y < (1u << fields.th); ++y)
    for (std::uint32_t x = 0; x < (1u << fields.tw); ++x) slm.writePixel(view, x, y, (3u * x + 5u * y) & 0xFFu);
}

Fields base_fields() {
  Fields fields;
  fields.tbp0 = 0x2C00;
  fields.tbw = 2;
  fields.psm = 0x14;
  fields.tw = 5;
  fields.th = 4;
  fields.cbp = 0x2D00;
  fields.csa = 0;
  fields.maxu = 31;
  fields.maxv = 15;
  return fields;
}

void check_state_matches_record() {
  SlmImage slm = full_slm();
  Fields fields = base_fields();
  seed_palette(slm.local, fields.cbp);
  seed_t4(slm.local, fields);
  const auto record = make_record(fields);
  DecodedTexture from_record;
  std::string error;
  require(decode_texture(slm, record.data(), record.size(), from_record, error) == TextureStatus::ok,
          "record path decodes");
  bool saw_state = false;
  for (std::uint32_t csa : {0u, 2u, 4u, 6u}) {
    fields.csa = csa;
    seed_t4(slm.local, fields);
    const auto state_record = make_record(fields);
    DecodedTexture latest_record;
    require(decode_texture(slm, state_record.data(), state_record.size(), latest_record, error) == TextureStatus::ok,
            "record path decodes at every CSA");
    TextureState state = make_state(fields);
    DecodedTexture from_state;
    require(decode_texture_state(slm, state, TextureDecodeOptions{}, from_state, error) == TextureStatus::ok,
            "state path decodes at every CSA");
    require(from_state.palette_via_tex2, "state path records the TEX2 palette load");
    require(from_state.rgba == latest_record.rgba, "state and record bytes agree at every CSA");
    require(from_state.key.textureHash == latest_record.key.textureHash &&
                from_state.key.paletteHash == latest_record.key.paletteHash,
            "state and record keys agree at every CSA");
    // The car-pass shape of the same state: TEX0.CLD = 1 in the TEX0 write itself, no TEX2 needed.
    TextureState cld1;
    cld1.tex0 = tex0_word(fields);
    cld1.tex1 = tex1_word(fields);
    cld1.clamp = clamp_word(fields);
    cld1.tex0_known = true;
    cld1.tex1_known = true;
    cld1.clamp_known = true;
    DecodedTexture from_cld1;
    require(decode_texture_state(slm, cld1, TextureDecodeOptions{}, from_cld1, error) == TextureStatus::ok,
            "state with TEX0.CLD 1 decodes");
    require(!from_cld1.palette_via_tex2 && from_cld1.rgba == latest_record.rgba,
            "state with TEX0.CLD 1 agrees with the record");
    // A T4 texture at CSA 0 is the measured group-0 load class; any other CSA uses the full-bank T8 class.
    require((fields.csa == 0u) == (hwgs::tex0::Psm(state.tex2) == 0x14u), "measured TEX2 load class");
    saw_state = true;
  }
  require(saw_state, "state cases ran");
  // The palette group selection is the tapped TEX0's CSA: two states that differ only there must decode differently.
  Fields group0 = base_fields();
  Fields group3 = base_fields();
  group3.csa = 3;
  DecodedTexture decoded0, decoded3;
  require(decode_texture_state(slm, make_state(group0), TextureDecodeOptions{}, decoded0, error) == TextureStatus::ok,
          "CSA 0 state decodes");
  require(decode_texture_state(slm, make_state(group3), TextureDecodeOptions{}, decoded3, error) == TextureStatus::ok,
          "CSA 3 state decodes");
  require(decoded0.rgba != decoded3.rgba, "the sampling CSA selects the palette group");
}

void check_t8_state() {
  SlmImage slm = full_slm();
  Fields fields = base_fields();
  fields.psm = 0x13;
  fields.csa = 2;
  seed_palette(slm.local, fields.cbp);
  seed_t8(slm.local, fields);
  const TextureState state = make_state(fields);
  require(hwgs::tex0::Psm(state.tex2) == 0x13u, "T8 texture uses the full-bank load class");
  DecodedTexture decoded;
  std::string error;
  require(decode_texture_state(slm, state, TextureDecodeOptions{}, decoded, error) == TextureStatus::ok,
          "T8 state decodes");
  require(decoded.palette_entries == 256, "T8 state decodes 256 entries");
  // The palette entry of index i is group (i >> 4) + CSA, entry i & 15 (the CSM1 sampling rule).
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 32; ++x) {
      const std::uint32_t index = (3u * x + 5u * y) & 0xFFu;
      const std::uint32_t group = std::min<std::uint32_t>((index >> 4) + fields.csa, 15u);
      const std::uint32_t expected = palette_color(group, index & 15u);
      std::uint32_t actual = 0;
      std::memcpy(&actual, decoded.rgba.data() + (static_cast<std::size_t>(y) * 32u + x) * 4u, 4);
      require(actual == expected, "T8 state palette group");
    }
  }
}

void check_tex1_and_clamp_from_state() {
  SlmImage slm = full_slm();
  Fields fields = base_fields();
  seed_palette(slm.local, fields.cbp);
  seed_t4(slm.local, fields);
  // The CLAMP region is the decoded rectangle (REGION_CLAMP, 16x8 at x 8, y 4).
  fields.wms = 2;
  fields.wmt = 2;
  fields.minu = 8;
  fields.maxu = 23;
  fields.minv = 4;
  fields.maxv = 11;
  TextureState state = make_state(fields);
  DecodedTexture decoded;
  std::string error;
  require(decode_texture_state(slm, state, TextureDecodeOptions{}, decoded, error) == TextureStatus::ok,
          "region state decodes");
  require(decoded.x0 == 8 && decoded.y0 == 4 && decoded.width == 16 && decoded.height == 8,
          "the state's CLAMP region is the decoded rectangle");
  // TEX1 is carried from the state: a mip record is refused unless the caller declares the level-0 fallback.
  state.tex1 = tex1_word(fields) | (1ull << 2);  // TEX1.MXL = 1
  DecodedTexture mip;
  require(decode_texture_state(slm, state, TextureDecodeOptions{}, mip, error) == TextureStatus::refused_state,
          "a state mip record is refused by default");
  TextureDecodeOptions fallback;
  fallback.level0_mip_fallback = true;
  require(decode_texture_state(slm, state, fallback, mip, error) == TextureStatus::ok, "level-0 fallback decodes");
  require(mip.level0_mip_fallback, "the level-0 fallback is recorded");
  require(mip.record.mxl == 1 && mip.record.mmag == 1 && mip.record.mmin == 1, "TEX1 fields come from the state");
}

void check_refusals() {
  SlmImage slm = full_slm();
  Fields fields = base_fields();
  seed_palette(slm.local, fields.cbp);
  seed_t4(slm.local, fields);
  DecodedTexture decoded;
  std::string error;
  auto expect = [&](TextureState state, TextureStatus status, const char *label) {
    DecodedTexture result;
    std::string reason;
    require(decode_texture_state(slm, state, TextureDecodeOptions{}, result, reason) == status, label);
    require(result.status == status, label);
    require(!result.detail.empty(), label);
  };
  TextureState state = make_state(fields);
  state.tex0_known = false;
  expect(state, TextureStatus::refused_state, "missing TEX0 refused");
  state = make_state(fields);
  state.tex1_known = false;
  expect(state, TextureStatus::refused_state, "missing TEX1 refused");
  state = make_state(fields);
  state.clamp_known = false;
  expect(state, TextureStatus::refused_state, "missing CLAMP refused");
  // CLD = 0 without a TEX2 write: the palette bank is unknown.
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2_known = false;
  expect(state, TextureStatus::refused_state, "CLD 0 without TEX2 refused");
  // A TEX2 that does not describe the same bank is refused, never guessed.
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x13, 5, 4, 1, 0, 0x2E00, 0, 0, 0, 1});
  expect(state, TextureStatus::refused_state, "TEX2 CBP mismatch refused");
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x13, 5, 4, 1, 0, fields.cbp, 2, 0, 0, 1});
  expect(state, TextureStatus::refused_state, "TEX2 CPSM mismatch refused");
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x13, 5, 4, 1, 0, fields.cbp, 0, 0, 3, 1});
  expect(state, TextureStatus::refused_state, "TEX2 CSA nonzero refused");
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x00, 5, 4, 1, 0, fields.cbp, 0, 0, 0, 1});
  expect(state, TextureStatus::refused_state, "TEX2 load class outside T4/T8 refused");
  state = make_state(fields);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x13, 5, 4, 1, 0, fields.cbp, 0, 0, 0, 0});
  expect(state, TextureStatus::refused_state, "TEX2 CLD 0 refused");
  // A T4 load class only fills group 0; a texture sampling another group is refused.
  Fields csa6 = fields;
  csa6.csa = 6;
  state = make_state(csa6);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x14, 5, 4, 1, 0, fields.cbp, 0, 0, 0, 1});
  expect(state, TextureStatus::refused_state, "T4 load with a sampled CSA above 0 refused");
  // The 16-entry load class cannot serve a T8 texture, whose indices span the whole 256-entry bank.
  Fields t8 = base_fields();
  t8.psm = 0x13;
  state = make_state(t8);
  state.tex0 &= ~(7ull << 61);
  state.tex2 = tex0_word(Fields{0x2C00, 2, 0x14, 5, 4, 1, 0, fields.cbp, 0, 0, 0, 1});
  expect(state, TextureStatus::refused_state, "T4 load class for a T8 texture refused");
  // A direct format needs no palette: CLD = 0 with no TEX2 decodes.
  Fields direct = base_fields();
  direct.psm = 0x00;
  direct.tbw = 4;
  std::vector<std::uint32_t> words(32u * 16u);
  for (std::uint32_t y = 0; y < 16; ++y)
    for (std::uint32_t x = 0; x < 32; ++x) words[y * 32u + x] = 0x80000000u | (y << 8) | (x << 16);
  slm.local.writeRect(hwgs::memory::BufferView{direct.tbp0, direct.tbw, hwgs::memory::Psm::Ct32}, 0, 0, 32, 16,
                      words.data());
  TextureState direct_state = make_state(direct);
  direct_state.tex0 &= ~(7ull << 61);
  direct_state.tex2_known = false;
  DecodedTexture direct_decoded;
  require(decode_texture_state(slm, direct_state, TextureDecodeOptions{}, direct_decoded, error) == TextureStatus::ok,
          "direct state decodes without a TEX2 load");
  require(!direct_decoded.palette_via_tex2 && direct_decoded.palette_entries == 0, "direct state has no palette");
  // The record path keeps its old refusals: CLD 0 without a state is still refused.
  Fields old = fields;
  old.cld = 0;
  const auto old_record = make_record(old);
  DecodedTexture refused;
  std::string reason;
  require(decode_texture(slm, old_record.data(), old_record.size(), refused, reason) == TextureStatus::refused_state,
          "record path still refuses CLD 0");
}

}  // namespace

int main() {
  try {
    check_state_matches_record();
    check_t8_state();
    check_tex1_and_clamp_from_state();
    check_refusals();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 texture state fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 texture state fixture: ok\n");
  return 0;
}
