// TASK-090 CoreGS 3.0, phase S3: controls for the first native HUD layer.
//
// Hand-authored inputs pin the measured rules:
//   * the stream's host->local IMAGE uploads reconstruct the GS local memory, and a T4 texture with a CT32 CSM1 palette
//     decodes through the same record decoder the world uses;
//   * the panel selection takes the PATH2 digit sprites of the attributed stream and the PATH1 plate quad of the tap,
//     merges the tap's SCISSOR by job id (the measured fix for the stale PATH2-only rectangle) and orders the plate
//     before the digits;
//   * the raster applies MODULATE, the normal ALPHA blend and the display buffer's CT16 model, and fills the mask;
//   * every refusal is named and counted (no tap, no plate, an unmeasured texture format, an untextured draw).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "coregs3_hud.hpp"
#include "coregs3_tap.hpp"

namespace {

using namespace gt4::coregs3;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// ---- GIF payload authoring (the tap records the VU1 emits) ----

void put_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

void put_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) bytes.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFF));
}

void put_qword(std::vector<std::uint8_t> &bytes, std::uint64_t lo, std::uint64_t hi) {
  put_u64(bytes, lo);
  put_u64(bytes, hi);
}

std::uint64_t packed_tag(std::uint32_t nloop, bool eop, bool pre, std::uint32_t prim, std::uint32_t nreg) {
  std::uint64_t tag = nloop & 0x7FFFu;
  if (eop) tag |= std::uint64_t{1} << 15;
  if (pre) tag |= std::uint64_t{1} << 46;
  tag |= static_cast<std::uint64_t>(prim & 0x7FFu) << 47;
  tag |= static_cast<std::uint64_t>(nreg & 0xFu) << 60;
  return tag;
}

// One A+D state record: FRAME, XYOFFSET, SCISSOR, CLAMP, TEX1, TEX0 (the measured plate job's set).
std::vector<std::uint8_t> plate_state_payload() {
  std::vector<std::uint8_t> bytes;
  const std::array<std::pair<std::uint32_t, std::uint64_t>, 6> writes = {{
      {0x4C, 0x00000000020A008Cu},  // FRAME: FBP 140, FBW 10, CT16
      {0x18, 0x0000720800006C08u},  // XYOFFSET
      {0x40, 0x01BF0000027F0000u},  // SCISSOR: full screen
      {0x08, 0x000000640014400Au},  // CLAMP: 0..81 x 0..25
      {0x14, 0x0000000000000060u},  // TEX1: MMAG/MMIN linear, MXL 0
      {0x06, 0x00073D055D30B9E0u},  // TEX0: TBP0 0x39E0, T8, TW 7, TH 5, TCC 1, CBP 0x39E8
  }};
  put_qword(bytes, packed_tag(static_cast<std::uint32_t>(writes.size()), true, false, 0, 1), 0xE);
  for (const auto &write : writes) put_qword(bytes, write.second, write.first);
  return bytes;
}

// One plate quad: the measured 4-vertex tri-strip with normalised ST, flat RGBA and XYOFFSET-relative positions.
std::vector<std::uint8_t> plate_quad_payload() {
  std::vector<std::uint8_t> bytes;
  // PRE tag with PRIM 0x05C (tri-strip, TME + ABE), NREG 3, descriptors ST, RGBAQ, XYZF2.
  std::uint64_t tag = 4;  // NLOOP
  tag |= std::uint64_t{1} << 15;
  tag |= std::uint64_t{1} << 46;
  tag |= static_cast<std::uint64_t>(0x05C) << 47;
  tag |= static_cast<std::uint64_t>(3) << 60;
  std::uint64_t regs = 0x2;
  regs |= std::uint64_t{0x1} << 4;
  regs |= std::uint64_t{0x4} << 8;
  put_qword(bytes, tag, regs);
  const auto f32 = [](float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return static_cast<std::uint64_t>(bits);
  };
  const auto qword_st = [&](float s, float t) {
    put_qword(bytes, f32(s) | (f32(t) << 32), f32(1.0f));
  };
  const auto qword_rgba = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    put_qword(bytes, static_cast<std::uint64_t>(r) | (static_cast<std::uint64_t>(g) << 32),
              static_cast<std::uint64_t>(b) | (static_cast<std::uint64_t>(a) << 32));
  };
  const auto qword_xyz = [&](std::uint32_t raw_x, std::uint32_t raw_y, std::uint32_t z) {
    put_qword(bytes, (static_cast<std::uint64_t>(raw_x) << 0) | (static_cast<std::uint64_t>(raw_y) << 32),
              static_cast<std::uint64_t>(z) << 4);
  };
  const std::uint32_t ofx = 0x6C08u, ofy = 0x7208u;
  const auto x4 = [&](float screen) { return ofx + static_cast<std::uint32_t>(std::lround(screen * 16.0f)); };
  const auto y4 = [&](float screen) { return ofy + static_cast<std::uint32_t>(std::lround(screen * 16.0f)); };
  qword_st(0.0f, 0.0f);
  qword_rgba(128, 128, 128, 128);
  qword_xyz(x4(550.5f), y4(377.5f), 0xFFFFFF);
  qword_st(0.641f, 0.0f);
  qword_rgba(128, 128, 128, 128);
  qword_xyz(x4(612.0f), y4(377.5f), 0xFFFFFF);
  qword_st(0.0f, 0.812f);
  qword_rgba(128, 128, 128, 128);
  qword_xyz(x4(550.5f), y4(403.625f), 0xFFFFFF);
  qword_st(0.641f, 0.812f);
  qword_rgba(128, 128, 128, 128);
  qword_xyz(x4(612.0f), y4(403.625f), 0xFFFFFF);
  return bytes;
}

// The tap file: header, one state record, one quad record.
void build_tap(std::vector<std::uint8_t> &data, std::vector<TapFileRecord> &records) {
  data.clear();
  records.clear();
  put_u32(data, kTapMagic);
  put_u32(data, 2);  // version
  put_u32(data, 0);
  put_u32(data, 0);
  const auto append = [&](const std::vector<std::uint8_t> &payload, std::uint64_t job) {
    TapFileRecord record;
    record.kind = 1;
    record.job = job;
    record.vblank = 101;
    record.bytes = static_cast<std::uint32_t>(payload.size());
    record.offset = data.size() + 24;
    records.push_back(record);
    put_u32(data, static_cast<std::uint32_t>(payload.size()));
    put_u32(data, 1);
    put_u64(data, job);
    put_u64(data, 101);
    data.insert(data.end(), payload.begin(), payload.end());
  };
  append(plate_state_payload(), 460652);
  append(plate_quad_payload(), 460652);
}

// ---- Synthetic stream batches ----

GsDecodeBatch present_blit(std::uint64_t vblank) {
  GsDecodeBatch batch;
  batch.vblank = vblank;
  batch.sequence = 1094;
  batch.path = 2;
  batch.full_screen = true;
  batch.vertices = 40;
  batch.drawing_vertices = 20;
  batch.last_job = 460600;
  batch.state.prim = 0x11E;  // sprite, TME
  batch.state.frame = 0x0000020A008Cu;  // FBP 140, FBW 10, CT16
  batch.state.tex0 = 0x1A40ull | (2ull << 14) | (7ull << 26) | (7ull << 30) | (1ull << 34);
  batch.state.tex1 = 0x60;
  batch.state.clamp = 0x000006FC009FC00Aull;
  batch.min_x = -0.5f;
  batch.max_x = 639.5f;
  batch.min_y = -0.5f;
  batch.max_y = 447.5f;
  return batch;
}

GsDecodeBatch panel_sprite(std::uint64_t vblank, std::uint32_t sequence, float x0, float y0, float x1, float y1,
                           std::uint32_t tbp0, std::uint32_t maxu, std::uint32_t maxv, std::uint64_t job,
                           std::uint64_t scissor) {
  GsDecodeBatch batch;
  batch.vblank = vblank;
  batch.sequence = sequence;
  batch.path = 2;
  batch.vertices = 2;
  batch.drawing_vertices = 1;
  batch.last_job = job;
  batch.full_screen = false;
  batch.state.prim = 0x15E;  // sprite, TME + ABE
  batch.state.frame = 0x020A008Cu;  // FBP 140, FBW 10, CT16
  batch.state.tex0 = tbp0 | (2ull << 14) | (0x14ull << 20) | (7ull << 26) | (6ull << 30) | (1ull << 34) |
                     (0x3A27ull << 37);  // T4, TCC 1, CT32 CSM1 palette
  batch.state.tex1 = 0x60;
  batch.state.clamp = 0x00000098001FC00Aull;  // 0..127 x 0..38
  batch.state.alpha = 0x44;                   // normal blend
  batch.state.scissor = scissor;
  batch.state.xyoffset = 0x0000720800006C08ull;
  batch.state.rgba = 0x3F80000080725C4Bull;
  (void)maxu;
  (void)maxv;
  GsDecodeVertex latch;
  latch.x = x0;
  latch.y = y0;
  latch.u = 91.0f;
  latch.v = 1.0f;
  latch.has_uv = true;
  latch.drawing = false;
  GsDecodeVertex draw;
  draw.x = x1;
  draw.y = y1;
  draw.u = 105.0f;
  draw.v = 19.0f;
  draw.has_uv = true;
  draw.drawing = true;
  batch.verts = {latch, draw};
  batch.min_x = x0;
  batch.max_x = x1;
  batch.min_y = y0;
  batch.max_y = y1;
  return batch;
}

GsDecodeBatch panel_clear(std::uint64_t vblank) {
  GsDecodeBatch batch;
  batch.vblank = vblank;
  batch.sequence = 1207;
  batch.path = 2;
  batch.full_screen = true;
  batch.vertices = 20;
  batch.drawing_vertices = 10;
  batch.last_job = 460700;
  batch.state.prim = 0x006;
  batch.state.frame = 0x020A008Cu | 0x00FFFFFFull << 32;
  batch.min_x = -0.5f;
  batch.max_x = 639.5f;
  batch.min_y = -0.5f;
  batch.max_y = 447.5f;
  return batch;
}

// ---- Tests ----

void test_stream_vram_and_texture() {
  // A 16x16 T4 texture at block 0x0100 (TBW 1) whose texel index is (x + y) & 0xF, and a 16-entry CSM1 CT32 palette at
  // block 0x0110 where entry i is (i * 8, 255 - i * 8, i * 4, 255).
  std::vector<std::uint8_t> texels(16 * 16 / 2, 0);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      const int index = y * 16 + x;
      const std::uint8_t value = static_cast<std::uint8_t>((x + y) & 0xF);
      if ((index & 1) == 0) texels[index / 2] |= value;
      else texels[index / 2] |= static_cast<std::uint8_t>(value << 4);
    }
  std::vector<std::uint8_t> palette(16 * 16 * 4, 0);
  for (int entry = 0; entry < 256; ++entry) {
    // CSM1 8-bit entry positions: x = (e & 7) + 8*((e >> 4) & 1), y = 2*(e >> 5) + ((e >> 3) & 1).
    const int x = (entry & 7) + 8 * ((entry >> 4) & 1);
    const int y = 2 * (entry >> 5) + ((entry >> 3) & 1);
    std::uint8_t *pixel = palette.data() + (static_cast<std::size_t>(y) * 16 + x) * 4u;
    pixel[0] = static_cast<std::uint8_t>(entry & 0xF);
    pixel[1] = static_cast<std::uint8_t>(15 - (entry & 0xF));
    pixel[2] = static_cast<std::uint8_t>(entry & 0x3);
    pixel[3] = 255;
  }
  std::vector<GsDecodeImageUpload> uploads;
  GsDecodeImageUpload texture_upload;
  texture_upload.vblank = 101;
  texture_upload.sequence = 1;
  texture_upload.path = 3;
  texture_upload.has_transfer_state = true;
  texture_upload.trxdir = 0;
  texture_upload.dbp = 0x0100;
  texture_upload.dbw = 1;
  texture_upload.dpsm = 0x14;
  texture_upload.rrw = 16;
  texture_upload.rrh = 16;
  texture_upload.data = texels;
  uploads.push_back(texture_upload);
  GsDecodeImageUpload palette_upload;
  palette_upload.vblank = 101;
  palette_upload.sequence = 2;
  palette_upload.path = 3;
  palette_upload.has_transfer_state = true;
  palette_upload.trxdir = 0;
  palette_upload.dbp = 0x0110;
  palette_upload.dbw = 1;
  palette_upload.dpsm = 0x00;
  palette_upload.rrw = 16;
  palette_upload.rrh = 16;
  palette_upload.data = palette;
  uploads.push_back(palette_upload);
  SlmImage slm;
  HudUploadCounters upload_counters;
  std::string error;
  check(build_hud_stream_vram(uploads, 101, false, slm, upload_counters, error),
        "the measured host->local uploads build the local memory");
  check(upload_counters.applied == 2 && upload_counters.refused == 0, "two uploads applied, none refused");

  HudDrawState state;
  state.tex0 = 0x0100ull | (1ull << 14) | (0x14ull << 20) | (4ull << 26) | (4ull << 30) | (1ull << 34) |
               (0x0110ull << 37);
  state.tex1 = 0x60;
  state.clamp = 2ull | (2ull << 2) | (15ull << 14) | (15ull << 34);
  HudTextureCache cache(slm);
  const HudTexture &texture = cache.get(state, error);
  check(texture.status == HudTextureStatus::ok, "the T4 + CT32 CSM1 texture decodes");
  check(texture.width == 16 && texture.height == 16, "the decode rectangle is the CLAMP region");
  if (texture.status == HudTextureStatus::ok) {
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        const std::uint8_t *pixel = texture.rgba.data() + (static_cast<std::size_t>(y) * 16 + x) * 4u;
        const int index = (x + y) & 0xF;
        check(pixel[0] == (index & 0xF) && pixel[1] == 15 - (index & 0xF) && pixel[2] == (index & 0x3),
              "the decoded texel samples the palette entry the T4 index selects");
        if (failures != 0) return;
      }
  }
  // A non-indexed format is refused with its named reason.
  HudDrawState direct = state;
  direct.tex0 = 0x0100ull | (1ull << 14) | (0x00ull << 20) | (4ull << 26) | (4ull << 30) | (1ull << 34);
  const HudTexture &refused = cache.get(direct, error);
  check(refused.status == HudTextureStatus::refused_format, "a CT32 HUD texture is refused as unmeasured");
  check(cache.refusals() == 1, "the refusal is counted");
  // A constant page the stream did NOT upload as texels (here the page bytes arrive through a CT32 transfer, the shape
  // of the night B-Spec font's palette bundle) keeps the S3 refusal, never drawn as a solid box.
  std::vector<std::uint8_t> empty_page(16 * 16 * 4, 0x00);
  std::vector<std::uint8_t> white_palette(16 * 16 * 4, 0xFF);
  std::vector<GsDecodeImageUpload> empty_uploads;
  GsDecodeImageUpload empty_upload;
  empty_upload.vblank = 101;
  empty_upload.has_transfer_state = true;
  empty_upload.dbp = 0x0120;
  empty_upload.dbw = 1;
  empty_upload.dpsm = 0x00;  // CT32: covers the blocks but is not a T4/T8 texel upload
  empty_upload.rrw = 16;
  empty_upload.rrh = 16;
  empty_upload.data = empty_page;
  empty_uploads.push_back(empty_upload);
  GsDecodeImageUpload empty_palette = empty_upload;
  empty_palette.dbp = 0x0130;
  empty_palette.dpsm = 0x00;
  empty_palette.rrw = 16;
  empty_palette.rrh = 16;
  empty_palette.data = white_palette;
  empty_uploads.push_back(empty_palette);
  SlmImage empty_slm;
  HudUploadCounters empty_counters;
  check(build_hud_stream_vram(empty_uploads, 101, false, empty_slm, empty_counters, error),
        "the empty page uploads build");
  HudDrawState empty_state = state;
  empty_state.tex0 = 0x0120ull | (1ull << 14) | (0x14ull << 20) | (3ull << 26) | (3ull << 30) | (1ull << 34) |
                      (0x0130ull << 37);
  empty_state.clamp = 2ull | (2ull << 2) | (7ull << 14) | (7ull << 34);
  HudTextureCache empty_cache(empty_slm);
  const HudTexture &constant = empty_cache.get(empty_state, error);
  check(constant.status == HudTextureStatus::refused_state, "a constant page without a texel upload is refused");
  check(error.find("font atlas is uploaded before the captured window") != std::string::npos,
        "the constant-page refusal names the measured reason");

  // TASK-090 HUD A-Spec: the same constant page sent as a T4 texel upload is real captured content (the A-Spec gear
  // box and map dot are solid pages), so it decodes as a legitimate solid texture instead of the S3 refusal.
  std::vector<std::uint8_t> t4_page(8 * 8 / 2, 0x00);
  std::vector<GsDecodeImageUpload> texel_uploads;
  GsDecodeImageUpload texel_upload = empty_upload;
  texel_upload.dpsm = 0x14;
  texel_upload.rrw = 8;
  texel_upload.rrh = 8;
  texel_upload.data = t4_page;
  texel_uploads.push_back(texel_upload);
  GsDecodeImageUpload texel_palette = empty_palette;
  texel_palette.dbp = 0x0130;
  texel_uploads.push_back(texel_palette);
  SlmImage texel_slm;
  HudUploadCounters texel_counters;
  check(build_hud_stream_vram(texel_uploads, 101, false, texel_slm, texel_counters, error),
        "the T4 constant page uploads build");
  HudTextureCache texel_cache(texel_slm);
  const HudTexture &solid = texel_cache.get(empty_state, error);
  check(solid.status == HudTextureStatus::ok, "a constant page with a captured T4 texel upload decodes");
  check(texel_cache.refusals() == 0, "the texel-sourced constant page is not counted as a refusal");
}

void test_panel_selection_and_merge() {
  const std::uint64_t scene = 100, stream = 101;
  std::vector<GsDecodeBatch> batches;
  batches.push_back(present_blit(stream));
  batches.push_back(panel_sprite(stream, 1102, 567.75f, 382.5f, 578.25f, 400.5f, 0x3A20u, 127, 38, 460653,
                                 0x004C002D027F0000ull));  // the stale PATH2-only scissor
  batches.push_back(panel_sprite(stream, 1096, 39.9f, 45.5f, 57.9f, 74.5f, 0x39A0u, 94, 127, 460645,
                                 0x004C002D027F0000ull));  // another element
  batches.push_back(panel_clear(stream));
  std::vector<std::uint8_t> tap_data;
  std::vector<TapFileRecord> tap_records;
  build_tap(tap_data, tap_records);
  HudPanel panel;
  std::string error;
  check(build_hud_panel(batches, tap_data, tap_records, scene, HudElement::speed, panel, error),
        "the speed panel builds from the synthetic stream and tap");
  check(panel.draws.size() == 2u, "the panel is the plate and the digit sprite");
  if (panel.draws.size() == 2u) {
    check(panel.draws[0].path1, "the plate (the lower job) draws first");
    check(!panel.draws[1].path1 && panel.draws[1].sequence == 1102u, "the digit sprite draws over the plate");
    const std::uint64_t scissor = panel.draws[1].state.scissor;
    check((scissor & 0x7FFu) == 0u && ((scissor >> 16) & 0x7FFu) == 639u && ((scissor >> 32) & 0x7FFu) == 0u &&
              ((scissor >> 48) & 0x7FFu) == 447u,
          "the merge replaces the stale PATH2 scissor with the plate job's full-screen write");
  }
  check(panel.counters.path2_stream_scissor_culled == 1, "the PATH2-only scissor would have culled the digit");
  check(panel.counters.path2_other == 1, "the other element is counted, not drawn");
  check(panel.counters.path1_selected == 1 && panel.counters.plate_quads == 1, "one plate quad selected");
  check(panel.counters.refusals == 0, "no refusal in the measured set");
  check(panel.draws[0].anchor == HudAnchor::right && panel.draws[1].anchor == HudAnchor::right,
        "the measured B-Spec speed plate and digits are right-anchored");
  check(panel.counters.anchor_right == 2 && panel.counters.anchor_left == 0 && panel.counters.anchor_centre == 0,
        "the anchor census counts the two right-anchored speed draws");

  // The census-driven `all` selection classifies each element: the synthetic left digit is left-anchored and the
  // bottom-right plate is right-anchored.
  HudPanel all;
  check(build_hud_panel(batches, tap_data, tap_records, scene, HudElement::all, all, error),
        "the all-element panel builds from the synthetic stream and tap");
  check(all.counters.anchor_left == 1 && all.counters.anchor_right == 2 && all.counters.anchor_centre == 0,
        "the all panel anchors the left digit left and the two bottom-right speed draws right");

  // A missing tap refuses the plate with its named reason.
  HudPanel without;
  std::vector<TapFileRecord> empty_records;
  const bool built = build_hud_panel(batches, tap_data, empty_records, scene, HudElement::speed, without, error);
  check(built && without.counters.path1_selected == 0, "without the tap the digits still build");
  check(without.counters.reasons.count("no tap: the element plate is PATH1 and cannot be drawn") == 1,
        "the missing plate is a named, counted refusal");
  (void)tap_data;
}

void test_raster_and_ct16() {
  // One 2x2 sprite sampled from a 4x4 T4 texture (nearest), blended over a known destination.
  SlmImage slm;
  HudDrawState state;
  state.prim = 0x15E;
  state.frame = 0x020A008Cu;
  state.tex0 = 0x0100ull | (1ull << 14) | (0x14ull << 20) | (2ull << 26) | (2ull << 30) | (1ull << 34) |
               (0x0110ull << 37);
  state.tex1 = 0;  // nearest/nearest: the measured subset allows it and the sampled texel is deterministic
  state.clamp = 2ull | (2ull << 2) | (3ull << 14) | (3ull << 34);
  state.alpha = 0x44;  // (Cs - Cd) * As + Cd
  state.scissor = 0x01BF0000027F0000ull;
  HudDraw draw;
  draw.state = state;
  draw.verts[0] = HudVertex{10.0f, 10.0f, 0.0f, 0.0f, 0xFF808080u, false};
  draw.verts[1] = HudVertex{12.0f, 12.0f, 2.0f, 2.0f, 0xFF808080u, true};
  draw.vertices = 2;
  draw.min_x = 10.0f;
  draw.max_x = 12.0f;
  draw.min_y = 10.0f;
  draw.max_y = 12.0f;
  HudPanel panel;
  panel.draws.push_back(draw);
  std::vector<std::uint8_t> rgba(16 * 16 * 4, 32);  // a uniform destination
  std::vector<std::uint8_t> mask;
  HudDrawCounters counters;
  std::string error;
  HudDrawOptions identity;  // the fixture target is also the 2D canvas: the measured 1x rule
  identity.canvas_width = 16;
  identity.canvas_height = 16;
  // A 4x4 T4 page: every texel is index 1 except host texel (3,3) = index 2 (byte 7's high nibble), so the decoded
  // page is not the measured "constant page" refusal; the sprite's nearest samples read texels 0..1, all index 1.
  std::vector<std::uint8_t> texels(4 * 4 / 2, 0x11);
  texels[7] = 0x21;
  std::vector<std::uint8_t> palette(16 * 16 * 4, 0);
  palette[4 + 0] = 200;
  palette[4 + 1] = 100;
  palette[4 + 2] = 50;
  palette[4 + 3] = 128;
  std::vector<GsDecodeImageUpload> uploads;
  GsDecodeImageUpload texture_upload;
  texture_upload.vblank = 1;
  texture_upload.has_transfer_state = true;
  texture_upload.dbp = 0x0100;
  texture_upload.dbw = 1;
  texture_upload.dpsm = 0x14;
  texture_upload.rrw = 4;
  texture_upload.rrh = 4;
  texture_upload.data = texels;
  uploads.push_back(texture_upload);
  GsDecodeImageUpload palette_upload = texture_upload;
  palette_upload.dbp = 0x0110;
  palette_upload.dpsm = 0x00;
  palette_upload.rrw = 16;
  palette_upload.rrh = 16;
  palette_upload.data = palette;
  uploads.push_back(palette_upload);
  HudUploadCounters upload_counters;
  check(build_hud_stream_vram(uploads, 1, false, slm, upload_counters, error), "the raster texture uploads build");
  HudTextureCache cache(slm);
  check(draw_hud_panel(panel, cache, rgba, 16, 16, identity, &mask, counters, error), "the panel rasters");
  check(counters.pixels == 4 && counters.masked_pixels == 4, "the 2x2 sprite writes four pixels");
  check(counters.refused == 0, "no raster refusal in the measured set");
  // MODULATE with vertex 128: 200*128>>7 = 200, 100, 50; alpha 128*128>>7 = 128. The destination is CT16-expanded 32,
  // and the normal blend is (Cs - Cd)*As + Cd = (200 - 32)*128 >> 7 + 32 = 200. The CT16 store keeps 5 bits: 200, 96, 48.
  const std::uint8_t *pixel = rgba.data() + (10 * 16 + 10) * 4u;
  check(pixel[0] == 200 && pixel[1] == 96 && pixel[2] == 48,
        "the blended pixel is the measured normal blend in the CT16 display buffer");
  check(mask[10 * 16 + 10] == 255, "the mask marks the written pixel");
  check(counters.blended == 4, "the blend is counted");

  // TASK-214: the same 2x2 canvas sprite rendered at a 3x output. Each target pixel centre maps back to the canvas, so
  // the box [10,12) x [10,12) covers target pixels 31..36 on both axes (36 pixels), and the first covered pixel is the
  // one whose centre lands exactly on the canvas 10.0.
  std::vector<std::uint8_t> scaled(48 * 48 * 4, 32);
  std::vector<std::uint8_t> scaled_mask;
  HudDrawCounters scaled_counters;
  HudDrawOptions three_x;
  three_x.canvas_width = 16;
  three_x.canvas_height = 16;
  check(draw_hud_panel(panel, cache, scaled, 48, 48, three_x, &scaled_mask, scaled_counters, error),
        "the panel rasters at the 3x output");
  check(scaled_counters.pixels == 36 && scaled_counters.masked_pixels == 36,
        "the 3x output covers 6x6 target pixels of the 2x2 canvas box");
  check(scaled_mask[31 * 48 + 31] == 255 && scaled_mask[36 * 48 + 36] == 255,
        "the 3x coverage starts at the target pixel whose centre is the canvas corner 10.0");
  check(scaled_mask[30 * 48 + 31] == 0 && scaled_mask[37 * 48 + 37] == 0,
        "the 3x coverage is half-open: the pixels outside the canvas box are not written");

  // TASK-090 HUD A-Spec: the untextured Gouraud triangle strip (a gauge needle). A 4-vertex strip covering the 8x8
  // canvas box 4..12 must write exactly 64 pixels (the shared diagonal edge once) and interpolate the vertex colours.
  HudDraw needle;
  needle.state.prim = 0x44;  // tri-strip, ABE on, TME off
  needle.state.alpha = 0x44;  // (Cs - Cd) * As + Cd
  needle.state.test = 0;     // ATE off (the measured needle state)
  needle.state.scissor = 0x01BF0000027F0000ull;
  needle.untextured_strip = true;
  needle.vertices = 4;
  needle.verts[0] = HudVertex{4.0f, 4.0f, 0.0f, 0.0f, 0x800000C8u, true};   // R 200, A 128
  needle.verts[1] = HudVertex{12.0f, 4.0f, 0.0f, 0.0f, 0x800000C8u, true};
  needle.verts[2] = HudVertex{4.0f, 12.0f, 0.0f, 0.0f, 0x80004040u, true};  // R 64
  needle.verts[3] = HudVertex{12.0f, 12.0f, 0.0f, 0.0f, 0x80004040u, true};
  needle.min_x = 4.0f;
  needle.max_x = 12.0f;
  needle.min_y = 4.0f;
  needle.max_y = 12.0f;
  HudPanel needle_panel;
  needle_panel.draws.push_back(needle);
  std::vector<std::uint8_t> needle_target(16 * 16 * 4, 32);
  std::vector<std::uint8_t> needle_mask;
  HudDrawCounters needle_counters;
  check(draw_hud_panel(needle_panel, cache, needle_target, 16, 16, identity, &needle_mask, needle_counters, error),
        "the untextured needle strip rasters");
  check(needle_counters.pixels == 64 && needle_counters.masked_pixels == 64,
        "the 8x8 needle quad writes 64 pixels with its shared edge once");
  check(needle_counters.refused == 0, "no needle raster refusal");
  // The strip runs from R 200 at the top to R 64 at the bottom; the normal blend over the 32 destination writes
  // (R - 32) * 128 / 128 + 32 = R, and the CT16 store keeps 5 bits: 200 at the top row and 64 at the bottom row.
  const std::uint8_t *needle_top = needle_target.data() + (4 * 16 + 4) * 4u;
  const std::uint8_t *needle_bottom = needle_target.data() + (11 * 16 + 4) * 4u;
  // The top row sits on the R 200 vertices; (11,11) has the barycentric weights 1/8 (TR 200), 3/4 (BR 64), 1/8 (BL 64)
  // = 81, stored as the CT16 5-bit value 80.
  check(needle_top[0] == 200 && needle_bottom[0] == 80, "the strip interpolates the vertex colours and blends once");
  check(needle_mask[4 * 16 + 4] == 255 && needle_mask[11 * 16 + 11] == 255, "the strip mask marks the covered pixels");

  // The measured needle ALPHA 0x64 = (Cs-Cd)*FIX+Cd with FIX 21/128 must blend instead of being refused: at the top
  // vertex, (200-32)*21>>7 + 32 = 59, stored as the CT16 value 56.
  HudDraw fix_needle = needle;
  fix_needle.state.alpha = 0x0000001500000064ull;
  HudPanel fix_panel;
  fix_panel.draws.push_back(fix_needle);
  std::vector<std::uint8_t> fix_target(16 * 16 * 4, 32);
  HudDrawCounters fix_counters;
  check(draw_hud_panel(fix_panel, cache, fix_target, 16, 16, identity, nullptr, fix_counters, error),
        "the FIX-family needle strip rasters");
  check(fix_counters.pixels == 64 && fix_counters.refused == 0, "the FIX-family strip writes the 8x8 quad");
  check(fix_target[(4 * 16 + 4) * 4u] == 56, "the FIX-family strip blends (Cs-Cd)*FIX+Cd");
}

void test_font_atlas_page() {
  // The GT4F sidecar: the measured GT4meter T4 page (128x128, 8192 bytes) plus its measured geometry. The page content
  // here is hand-authored: index 0 everywhere, index 1 in the top-left 4x4 texels (so the decoded page is not constant).
  constexpr std::uint32_t tbp0 = 0x3A20u, cbp = 0x3A27u;
  std::vector<std::uint8_t> linear(128 * 128 / 2, 0);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * 128u + static_cast<std::size_t>(x);
      if ((index & 1u) == 0) linear[index / 2] |= 1;
      else linear[index / 2] |= 0x10;
    }
  hwgs::memory::LocalMemory page_memory;
  hwgs::memory::hostToLocal(page_memory, hwgs::memory::BufferView{0, 2, hwgs::memory::Psm::T4},
                            hwgs::memory::TransferRect{0, 0, 128, 128}, std::span<const std::uint8_t>(linear));
  std::vector<std::uint8_t> page(page_memory.bytes().begin(), page_memory.bytes().begin() + 8192);

  std::vector<std::uint8_t> file;
  put_u32(file, 0x46345447u);  // 'GT4F'
  put_u32(file, 1u);
  put_u32(file, tbp0);
  put_u32(file, 2u);  // TBW
  put_u32(file, 7u);  // TW
  put_u32(file, 6u);  // TH
  put_u64(file, 0x00000098001FC00Aull);  // CLAMP 0..127 x 0..38
  put_u32(file, 8192u);
  put_u32(file, 0u);
  file.insert(file.end(), page.begin(), page.end());
  HudFontAtlas atlas;
  std::string error;
  check(parse_hud_font_atlas(file, atlas, error), "the measured GT4F font atlas parses");
  check(atlas.tbp0 == tbp0 && atlas.tw == 7u && atlas.th == 6u && atlas.page.size() == 8192u,
        "the atlas carries the measured page and geometry");
  // Every field outside the measured geometry is refused by name.
  HudFontAtlas scratch;
  std::vector<std::uint8_t> bad = file;
  bad[0] = 0;
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("bad magic") != std::string::npos,
        "a bad GT4F magic is refused");
  bad = file;
  bad[4] = 2;
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("version") != std::string::npos,
        "an unknown GT4F version is refused");
  bad = file;
  bad[36] = 1;
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("reserved") != std::string::npos,
        "a nonzero reserved word is refused");
  bad = file;
  bad[16] = 6;  // TW 6
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("geometry") != std::string::npos,
        "a page geometry that is not the measured 128x64 is refused");
  bad = file;
  bad[26] = 0;  // CLAMP maxu bits 16..23: 127 becomes 3
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("CLAMP") != std::string::npos,
        "a CLAMP region outside the measured 0..127 x 0..38 is refused");
  bad = file;
  bad[33] = 0;  // page_bytes 0x2000 -> 0x0000
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("page size") != std::string::npos,
        "a page size other than 8192 is refused");
  bad = file;
  bad.resize(file.size() - 4);
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("truncated") != std::string::npos,
        "a truncated page is refused");
  bad = file;
  bad[8] = 0xF0;  // TBP0 0x3FF0: the 32-block page would run past the 4 MiB image
  bad[9] = 0x3F;
  check(!parse_hud_font_atlas(bad, scratch, error) && error.find("outside the 4 MiB image") != std::string::npos,
        "a page outside the local memory is refused");

  // Apply: the palette bank the draw samples (the 64 bytes at CBP) is preserved across the page write, and the page's
  // blocks become covered.
  SlmImage slm;
  std::vector<std::uint8_t> palette_bank(64);
  for (std::size_t index = 0; index < palette_bank.size(); ++index)
    palette_bank[index] = static_cast<std::uint8_t>(index * 3u + 1u);
  std::copy(palette_bank.begin(), palette_bank.end(),
            slm.local.bytes().begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(cbp) * 256u));
  check(apply_hud_font_atlas(atlas, slm, atlas.tbp0, cbp, error), "the font atlas applies to the local memory");
  std::vector<std::uint8_t> expected_page = page;
  std::copy(palette_bank.begin(), palette_bank.end(),
            expected_page.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(cbp - tbp0)) * 256u));
  check(std::equal(expected_page.begin(), expected_page.end(),
                   slm.local.bytes().begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(tbp0) * 256u)),
        "the page bytes land at the atlas TBP0 (the preserved bank excepted)");
  check(std::equal(palette_bank.begin(), palette_bank.end(),
                   slm.local.bytes().begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(cbp) * 256u)),
        "the 16-entry CSM1 palette bank is preserved across the page write");
  check(slm.covers(tbp0) && slm.covers(tbp0 + 31u) && !slm.covers(tbp0 + 32u),
        "the atlas covers exactly its 32 blocks");
  // The GT4meter page rotates through VRAM: the A-Spec digits sample it at another page than the sidecar's TBP0.
  SlmImage rotated;
  check(apply_hud_font_atlas(atlas, rotated, tbp0 + 0x40u, cbp, error),
        "the atlas applies at a rotated page (the A-Spec TBP0 rule)");
  check(rotated.covers(tbp0 + 0x40u) && rotated.covers_texels(tbp0 + 0x40u) && !rotated.covers(tbp0),
        "the rotated page is covered as texel content and the original page is untouched");
  HudFontAtlas out_of_range = atlas;
  out_of_range.tbp0 = 0x3FF0u;
  SlmImage other;
  check(!apply_hud_font_atlas(out_of_range, other, out_of_range.tbp0, cbp, error) &&
            error.find("outside the 4 MiB image") != std::string::npos,
        "an atlas page outside the image is refused on apply");

  // The applied page decodes through the same T4 record path the digits use; the 4x4 index-1 block reads the palette
  // bank at CBP, so the decoded page is not the constant-page refusal.
  // The hand palette mirrors the measured GT4meter bank: white RGB with the alpha carrying the glyph coverage.
  std::vector<std::uint8_t> palette(16 * 16 * 4, 0);
  for (int entry = 0; entry < 256; ++entry) {
    const int x = (entry & 7) + 8 * ((entry >> 4) & 1);
    const int y = 2 * (entry >> 5) + ((entry >> 3) & 1);
    std::uint8_t *pixel = palette.data() + (static_cast<std::size_t>(y) * 16 + x) * 4u;
    pixel[0] = 255;
    pixel[1] = 255;
    pixel[2] = 255;
    pixel[3] = static_cast<std::uint8_t>(entry == 0 ? 0 : 128);
  }
  SlmImage decoded_slm;
  std::copy(page.begin(), page.end(),
            decoded_slm.local.bytes().begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(tbp0) * 256u));
  hwgs::memory::hostToLocal(decoded_slm.local, hwgs::memory::BufferView{cbp, 1, hwgs::memory::Psm::Ct32},
                            hwgs::memory::TransferRect{0, 0, 16, 16}, std::span<const std::uint8_t>(palette));
  decoded_slm.cover(vram::Window{tbp0, 32u});
  decoded_slm.cover(vram::Window{cbp, 4u});
  HudDrawState state;
  state.tex0 = tbp0 | (2ull << 14) | (0x14ull << 20) | (7ull << 26) | (6ull << 30) | (1ull << 34) |
               (static_cast<std::uint64_t>(cbp) << 37);
  state.tex1 = 0x60;
  state.clamp = 0x00000098001FC00Aull;
  HudTextureCache cache(decoded_slm);
  cache.set_font_atlas_page(tbp0);
  const HudTexture &texture = cache.get(state, error);
  check(texture.status == HudTextureStatus::ok, "the applied T4 font page decodes through the record path");
  if (texture.status == HudTextureStatus::ok) {
    check(texture.width == 128 && texture.height == 39, "the decoded font page is the measured 128x39 region");
    const std::uint8_t *texel = texture.rgba.data();
    check(texel[0] == 255 && texel[1] == 255 && texel[2] == 255 && texel[3] == 128,
          "the index-1 glyph texel samples the preserved palette bank");
  }
  check(cache.refusals() == 0, "the applied font page is not refused");
  // Without the atlas flag (the stream-only A/B), the same white-alpha page keeps the S3 RGB-constant refusal.
  HudTextureCache stream_only(decoded_slm);
  const HudTexture &refused_again = stream_only.get(state, error);
  check(refused_again.status == HudTextureStatus::refused_state,
        "a white-alpha page without the atlas flag keeps the S3 constant-page refusal");
}

void test_panel_census() {
  // TASK-090 HUD A-Spec: the census enumerates the panel range without an element selection, with the PATH1 plate of
  // the tap and every PATH2 sprite, each carrying its captured state.
  const std::uint64_t stream = 101;
  std::vector<GsDecodeBatch> batches;
  batches.push_back(present_blit(stream));
  batches.push_back(panel_sprite(stream, 1102, 567.75f, 382.5f, 578.25f, 400.5f, 0x3A20u, 127, 38, 460653,
                                 0x004C002D027F0000ull));
  batches.push_back(panel_sprite(stream, 1096, 39.9f, 45.5f, 57.9f, 74.5f, 0x39A0u, 94, 127, 460645,
                                 0x004C002D027F0000ull));
  batches.push_back(panel_clear(stream));
  std::vector<std::uint8_t> tap_data;
  std::vector<TapFileRecord> tap_records;
  build_tap(tap_data, tap_records);
  HudCensus census;
  std::string error;
  check(build_hud_census(batches, tap_data, tap_records, stream, census, error), "the panel census builds");
  check(census.path2_batches == 2u && census.path2_drawing == 2u, "the census counts the two panel sprites");
  check(census.path1_display_draws == 1u && census.path1_axis_aligned == 1u, "the census counts the axis-aligned plate");
  check(census.entries.size() == 3u, "the census has the two sprites and the plate");
  if (census.entries.size() == 3u) {
    check(!census.entries[0].path1 && census.entries[0].sequence == 1096u, "the lower-job sprite comes first");
    check(census.entries[1].path1 && census.entries[1].axis_aligned && census.entries[1].state.tex0_psm() == 0x13u,
          "the plate quad is the PATH1 entry, axis aligned, with its T8 state");
    check(!census.entries[2].path1 && census.entries[2].sequence == 1102u &&
              census.entries[2].state.tex0_tbp0() == 0x3A20u,
          "the digit sprite carries its captured TEX0 page");
  }
  // No present blit: the census refuses with a named reason instead of guessing a range.
  HudCensus missing;
  std::vector<GsDecodeBatch> no_present(batches.begin() + 1, batches.end());
  check(!build_hud_census(no_present, tap_data, tap_records, stream, missing, error),
        "a census without the present blit is refused");
}

// TASK-214/TASK-080: the aspect layout and the measured element anchor rule.
void test_aspect_layout_and_anchor_rule() {
  // 16:9 at the 3x validation scale: the captured mapping (scale 3 x 3) and no anchor translation. This is the identity
  // the existing HUD A/Bs require.
  const HudAspectLayout reference = hud_aspect_layout(640.0, 448.0, 1920, 1344, kHudReferenceAspect);
  check(reference.anchored, "the 16:9 target aspect is an anchored layout");
  check(std::abs(reference.virtual_width - 640.0) < 1e-9, "the 16:9 virtual canvas is the captured 640-wide canvas");
  check(std::abs(reference.scale_x - 3.0) < 1e-9 && std::abs(reference.scale_y - 3.0) < 1e-9,
        "the 3x validation scale maps 640x448 to 1920x1344");
  check(reference.anchor_offset(HudAnchor::left) == 0.0 && reference.anchor_offset(HudAnchor::centre) == 0.0 &&
            reference.anchor_offset(HudAnchor::right) == 0.0,
        "the 16:9 render has no anchor translation");

  // 1080p 16:9: a square-pixel 16:9 output. The vertical scale is the output height over the 480-unit HUD canvas.
  const HudAspectLayout hd = hud_aspect_layout(640.0, 448.0, 1920, 1080, kHudReferenceAspect);
  check(std::abs(hd.scale_x - 3.0) < 1e-9 && std::abs(hd.scale_y - 1080.0 / 448.0) < 1e-9,
        "1080p 16:9 keeps the captured layout (x 3, y 1080/448)");

  // 3440x1440 is 2.3889:1, not the nominal 21:9. The exact output aspect widens the canvas to 860 captured units and
  // keeps the UI's square design: scale_x 4, scale_y 1440/448, right anchor translation 220, centre 110.
  const double ultrawide_aspect = 3440.0 / 1440.0;
  const HudAspectLayout ultrawide = hud_aspect_layout(640.0, 448.0, 3440, 1440, ultrawide_aspect);
  check(std::abs(ultrawide.virtual_width - 860.0) < 1e-9, "3440x1440 widens the HUD canvas to 860 units");
  check(std::abs(ultrawide.scale_x - 4.0) < 1e-9 && std::abs(ultrawide.scale_y - 1440.0 / 448.0) < 1e-9,
        "the ultrawide output maps the canvas with x 4 and y 1440/448");
  check(ultrawide.anchor_offset(HudAnchor::left) == 0.0 && std::abs(ultrawide.anchor_offset(HudAnchor::centre) - 110.0) < 1e-9 &&
            std::abs(ultrawide.anchor_offset(HudAnchor::right) - 220.0) < 1e-9,
        "the edge/centre anchors translate by 0/110/220 captured units");
  // The nominal 21:9 value at the 3440x1440 output would stretch the UI by 2.4%: the rule is parameterised by the exact
  // output aspect, and the tool's `auto` mode passes that.
  const HudAspectLayout nominal = hud_aspect_layout(640.0, 448.0, 3440, 1440, 21.0 / 9.0);
  check(std::abs(nominal.virtual_width - 840.0) < 1e-9 && nominal.scale_x > ultrawide.scale_x + 0.09,
        "the nominal 21:9 ratio is 840 canvas units; a 3440x1440 output must pass its exact aspect");

  // No target aspect: the captured mapping of the previous tool (byte identity).
  const HudAspectLayout captured = hud_aspect_layout(640.0, 448.0, 1920, 1344, 0.0);
  check(!captured.anchored && std::abs(captured.scale_x - 3.0) < 1e-9 && std::abs(captured.scale_y - 3.0) < 1e-9,
        "without a target aspect the mapping is the captured canvas stretch");

  // The measured A-Spec element rule (1x census boxes of scene 7600).
  check(hud_classify_anchor(0x3C20u, 33.8f, 65.2f, 640.0) == HudAnchor::left, "the position plate is left-anchored");
  check(hud_classify_anchor(0x3C40u, 39.9f, 57.9f, 640.0) == HudAnchor::left, "the position digits are left-anchored");
  check(hud_classify_anchor(0x3B80u, 25.5f, 160.5f, 640.0) == HudAnchor::left, "the minimap outline is left-anchored");
  check(hud_classify_anchor(0x3B60u, 102.6f, 119.1f, 640.0) == HudAnchor::left,
        "the minimap sign icon stays with its block (the page rule, not the 96 px margin)");
  check(hud_classify_anchor(0x3000u, 531.5f, 596.0f, 640.0) == HudAnchor::right, "the Best/Last times are right-anchored");
  check(hud_classify_anchor(0x3000u, 297.5f, 364.0f, 640.0) == HudAnchor::centre, "the lap timer is centre-anchored");
  check(hud_classify_anchor(0x3000u, 260.5f, 378.2f, 640.0) == HudAnchor::centre, "FULL THROTTLE is centre-anchored");
  check(hud_classify_anchor(0x3DE0u, 348.0f, 445.5f, 640.0) == HudAnchor::centre, "the tachometer dial is centre-anchored");
  check(hud_classify_anchor(0x3DA0u, 193.5f, 291.0f, 640.0) == HudAnchor::centre,
        "the speedometer dial is centre-anchored");
  // The geometric fallback for an unclassified page: the game's wideOffset distance bounds the edge block.
  check(hud_classify_anchor(0x3999u, 550.5f, 612.0f, 640.0) == HudAnchor::right, "a measured right plate falls back right");
  check(hud_classify_anchor(0x3999u, 21.8f, 129.0f, 640.0) == HudAnchor::left, "a measured left block falls back left");
  check(hud_classify_anchor(0x3999u, 200.0f, 400.0f, 640.0) == HudAnchor::centre, "a central draw falls back centre");
  check(std::string(hud_anchor_name(HudAnchor::left)) == "left" &&
            std::string(hud_anchor_name(HudAnchor::centre)) == "centre" &&
            std::string(hud_anchor_name(HudAnchor::right)) == "right",
        "the anchor names round-trip");
}

// The anchored raster on a synthetic panel: three 2x2 sprites at the left edge, the centre and the right edge, drawn at
// 640x448 (the captured mapping) and at 3440x1440 (the ultrawide anchoring). The covered pixel ranges are the exact
// half-open rule the renderer uses.
void test_aspect_raster() {
  // A 4x4 T4 page with a 16-entry CSM1 palette (the same hand-authored texture the S3 raster test uses): every sampled
  // texel is index 1 with RGBA (200, 100, 50, 128).
  std::vector<std::uint8_t> texels(4 * 4 / 2, 0x11);
  texels[7] = 0x21;
  std::vector<std::uint8_t> palette(16 * 16 * 4, 0);
  palette[4 + 0] = 200;
  palette[4 + 1] = 100;
  palette[4 + 2] = 50;
  palette[4 + 3] = 128;
  std::vector<GsDecodeImageUpload> uploads;
  GsDecodeImageUpload texture_upload;
  texture_upload.vblank = 1;
  texture_upload.has_transfer_state = true;
  texture_upload.dbp = 0x0100;
  texture_upload.dbw = 1;
  texture_upload.dpsm = 0x14;
  texture_upload.rrw = 4;
  texture_upload.rrh = 4;
  texture_upload.data = texels;
  uploads.push_back(texture_upload);
  GsDecodeImageUpload palette_upload = texture_upload;
  palette_upload.dbp = 0x0110;
  palette_upload.dpsm = 0x00;
  palette_upload.rrw = 16;
  palette_upload.rrh = 16;
  palette_upload.data = palette;
  uploads.push_back(palette_upload);
  SlmImage slm;
  HudUploadCounters upload_counters;
  std::string error;
  check(build_hud_stream_vram(uploads, 1, false, slm, upload_counters, error), "the anchor texture uploads build");
  HudTextureCache cache(slm);

  HudDrawState state;
  state.prim = 0x15E;
  state.frame = 0x020A008Cu;
  state.tex0 = 0x0100ull | (1ull << 14) | (0x14ull << 20) | (2ull << 26) | (2ull << 30) | (1ull << 34) |
               (0x0110ull << 37);
  state.tex1 = 0;
  state.clamp = 2ull | (2ull << 2) | (3ull << 14) | (3ull << 34);
  state.alpha = 0x44;
  // A synthetic scissor beyond the canvas on every side: the anchor test isolates the mapping from the measured
  // full-screen scissor's own edge convention (the captured panel uses 0..639 x 0..447; no element touches its edge).
  state.scissor = (2047ull << 16) | (2047ull << 48);
  const auto sprite = [&](float x0, float x1, HudAnchor anchor) {
    HudDraw draw;
    draw.state = state;
    draw.verts[0] = HudVertex{x0, 10.0f, 0.0f, 0.0f, 0xFF808080u, false};
    draw.verts[1] = HudVertex{x1, 12.0f, 2.0f, 2.0f, 0xFF808080u, true};
    draw.vertices = 2;
    draw.min_x = x0;
    draw.max_x = x1;
    draw.min_y = 10.0f;
    draw.max_y = 12.0f;
    draw.anchor = anchor;
    return draw;
  };
  HudPanel panel;
  panel.draws.push_back(sprite(0.0f, 2.0f, HudAnchor::left));
  panel.draws.push_back(sprite(319.0f, 321.0f, HudAnchor::centre));
  panel.draws.push_back(sprite(638.0f, 640.0f, HudAnchor::right));

  // The captured mapping at 640x448: left pixels 0..1, centre 319..320, right 638..639 (the half-open coverage rule).
  std::vector<std::uint8_t> base(640 * 448 * 4, 32);
  std::vector<std::uint8_t> base_mask;
  HudDrawCounters base_counters;
  HudDrawOptions captured;
  check(draw_hud_panel(panel, cache, base, 640, 448, captured, &base_mask, base_counters, error),
        "the anchor panel rasters at 640x448");
  check(base_mask[10 * 640 + 0] == 255 && base_mask[10 * 640 + 1] == 255 && base_mask[10 * 640 + 2] == 0,
        "the left sprite covers pixels 0..1 of the captured canvas");
  check(base_mask[10 * 640 + 319] == 255 && base_mask[10 * 640 + 320] == 255, "the centre sprite stays at 319..320");
  check(base_mask[10 * 640 + 638] == 255 && base_mask[10 * 640 + 639] == 255, "the right sprite reaches pixel 639");

  // The same panel at 3440x1440 with its exact aspect: left stays at the left edge, the centre moves by half of the
  // added width (110 captured units = 440 output pixels), the right edge stays at the right edge.
  std::vector<std::uint8_t> ultrawide_image(3440 * 1440 * 4, 32);
  std::vector<std::uint8_t> ultrawide_mask;
  HudDrawCounters ultrawide_counters;
  HudDrawOptions options;
  options.target_aspect = 3440.0 / 1440.0;
  check(draw_hud_panel(panel, cache, ultrawide_image, 3440, 1440, options, &ultrawide_mask, ultrawide_counters,
                       error),
        "the anchor panel rasters at 3440x1440");
  // x 2..9 (the [0,2) canvas box at scale 4), y 34..39 (the [10,12) box at 1440/448).
  check(ultrawide_mask[34 * 3440 + 2] == 255 && ultrawide_mask[34 * 3440 + 9] == 255,
        "the left sprite covers the left-edge output pixels 2..9");
  check(ultrawide_mask[34 * 3440 + 1] == 0 && ultrawide_mask[34 * 3440 + 10] == 0,
        "the left sprite does not cover the pixels outside its canvas box");
  check(ultrawide_mask[34 * 3440 + 1718] == 255 && ultrawide_mask[34 * 3440 + 1725] == 255,
        "the centre sprite covers 1718..1725, centred on 1720");
  check(ultrawide_mask[34 * 3440 + 1717] == 0 && ultrawide_mask[34 * 3440 + 1726] == 0,
        "the centre sprite is not stretched");
  check(ultrawide_mask[34 * 3440 + 3434] == 255 && ultrawide_mask[34 * 3440 + 3439] == 255,
        "the right sprite reaches the last output column");
  check(ultrawide_mask[34 * 3440 + 3433] == 0, "the right sprite does not start before its anchored box");
  // The left and centre boxes cover 8x6 output pixels each; the right box ends exactly at the canvas edge, so its last
  // two sample columns lie beyond the output (6x6 pixels are written).
  check(ultrawide_counters.pixels == 2 * 8 * 6 + 6 * 6, "the three 2x2 canvas boxes cover 8x6, 8x6 and 6x6 pixels at x4");

  // The 16:9 target aspect is byte-identical to the captured mapping, including at 1920x1344 (anamorphic output).
  std::vector<std::uint8_t> reference(1920 * 1344 * 4, 32);
  std::vector<std::uint8_t> reference_mask;
  HudDrawCounters reference_counters;
  HudDrawOptions reference_options;
  reference_options.target_aspect = kHudReferenceAspect;
  check(draw_hud_panel(panel, cache, reference, 1920, 1344, reference_options, &reference_mask, reference_counters,
                       error),
        "the 16:9 anchored panel rasters at 1920x1344");
  std::vector<std::uint8_t> plain(1920 * 1344 * 4, 32);
  std::vector<std::uint8_t> plain_mask;
  HudDrawCounters plain_counters;
  HudDrawOptions plain_options;
  check(draw_hud_panel(panel, cache, plain, 1920, 1344, plain_options, &plain_mask, plain_counters, error),
        "the captured panel rasters at 1920x1344");
  check(reference == plain && reference_mask == plain_mask,
        "the 16:9 target aspect is byte-identical to the captured mapping");

  // The aspect range refusals are named, never silently clamped.
  std::vector<std::uint8_t> narrow(640 * 448 * 4, 32);
  HudDrawCounters narrow_counters;
  HudDrawOptions narrow_options;
  narrow_options.target_aspect = 4.0 / 3.0;
  check(!draw_hud_panel(panel, cache, narrow, 640, 448, narrow_options, nullptr, narrow_counters, error) &&
            error.find("narrower than the captured 16:9") != std::string::npos,
        "a target aspect narrower than 16:9 is refused by name");
  HudDrawOptions too_wide = narrow_options;
  too_wide.target_aspect = 7.0;
  check(!draw_hud_panel(panel, cache, narrow, 640, 448, too_wide, nullptr, narrow_counters, error) &&
            error.find("6.0") != std::string::npos,
        "a target aspect above 6.0 is refused by name");

  // The anchor report is the measurement instrument: a header plus one line per draw with its anchor and output box.
  const HudAspectLayout ultrawide_layout =
      hud_aspect_layout(640.0, 448.0, 3440, 1440, options.target_aspect);
  const std::string report = hud_anchor_report(panel, ultrawide_layout);
  check(report.rfind("index,path,sequence,job,anchor,tbp0,", 0u) == 0u, "the anchor report has its documented header");
  check(report.find(",left,0x0100,") != std::string::npos && report.find(",centre,0x0100,") != std::string::npos &&
            report.find(",right,0x0100,") != std::string::npos,
        "the anchor report names each draw's anchor class");
  check(report.find(",220.000,3432.000,3440.000,") != std::string::npos,
        "the right draw's output box carries the 220-unit translation at scale 4");
}

void test_names() {
  HudElement element;
  check(parse_hud_element("speed", element) && element == HudElement::speed, "the speed element parses");
  check(!parse_hud_element("gear", element), "an unimplemented element is refused, not guessed");
  check(std::string(hud_element_name(HudElement::speed)) == "speed", "the element name round-trips");
}

}  // namespace

int main() {
  test_names();
  test_stream_vram_and_texture();
  test_font_atlas_page();
  test_panel_selection_and_merge();
  test_panel_census();
  test_raster_and_ct16();
  test_aspect_layout_and_anchor_rule();
  test_aspect_raster();
  if (failures != 0) {
    std::fprintf(stderr, "%d HUD check(s) failed\n", failures);
    return 1;
  }
  std::printf("coregs3 HUD checks passed\n");
  return 0;
}
