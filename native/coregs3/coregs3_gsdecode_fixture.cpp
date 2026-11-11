// TASK-090 CoreGS 3.0, phase S2: controls for the `.gt4gs` decoder and census (coregs3_gsdecode.hpp).
//
// The fixture writes real `.gt4gs` files through the S1 writer and reads them back through the S1 reader and the S2
// walker, so the three independent pieces are pinned against each other: PACKED A+D register writes and sprite kicks,
// the REGLIST sprite layout ([UV, XYZF3, UV, XYZF2]), an IMAGE upload whose tag and data are split across two capture
// records, the 12.4 screen mapping, the full-screen rule, the V -> V-1 attribution and every named refusal.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "coregs3_gsdecode.hpp"
#include "coregs3_gsstream.hpp"
#include "coregs3_gsstream_file.hpp"

namespace {

using namespace gt4::coregs3;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void put(std::vector<std::uint8_t> &out, std::uint64_t lo, std::uint64_t hi) {
  for (int index = 0; index < 8; ++index) out.push_back(static_cast<std::uint8_t>((lo >> (8 * index)) & 0xFF));
  for (int index = 0; index < 8; ++index) out.push_back(static_cast<std::uint8_t>((hi >> (8 * index)) & 0xFF));
}

void put_tag(std::vector<std::uint8_t> &out, std::uint32_t nloop, bool eop, bool pre, std::uint32_t prim,
             std::uint32_t format, std::uint32_t nreg, std::uint64_t registers) {
  std::uint64_t lo = nloop & 0x7FFFu;
  if (eop) lo |= 1ull << 15;
  if (pre) lo |= 1ull << 46;
  lo |= static_cast<std::uint64_t>(prim & 0x7FFu) << 47;
  lo |= static_cast<std::uint64_t>(format & 3u) << 58;
  lo |= static_cast<std::uint64_t>(nreg & 0xFu) << 60;
  put(out, lo, registers);
}

// 64-bit words one per A+D write (descriptor 0xE, address in the high word).
void put_ad(std::vector<std::uint8_t> &out, std::uint32_t address, std::uint64_t data) {
  put(out, data, address & 0x7F);
}

void write_file(const std::string &path, const std::vector<std::vector<std::uint8_t>> &records,
                std::uint64_t vblank) {
  GsStreamConfig config;
  config.path = path;
  config.range = true;
  config.first_vblank = vblank;
  config.last_vblank = vblank;
  config.max_frames = 8;
  std::string error;
  if (!configure_gsstream(config, error)) {
    std::fprintf(stderr, "configure: %s\n", error.c_str());
    ++failures;
    return;
  }
  gsstream_frame(vblank);
  for (const std::vector<std::uint8_t> &record : records) {
    const std::size_t qwords = record.size() / 16;
    gsstream_packet(2, record.data(), record.size(), static_cast<std::uint32_t>(qwords), false, 4242, 0x004AB548,
                    0x00001000);
  }
  gsstream_shutdown();
  gsstream_reset_for_test();
}

bool load_and_census(const std::string &path, GsStreamCensus &census, std::vector<GsDecodeBatch> &batches,
                     std::vector<GsDecodeImageUpload> &uploads) {
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  if (!read_gs_stream_file(path, data, header, records, error)) {
    std::fprintf(stderr, "read: %s\n", error.c_str());
    return false;
  }
  if (!census_gs_stream(data, header, records, census, batches, uploads, error)) {
    std::fprintf(stderr, "census: %s\n", error.c_str());
    return false;
  }
  return true;
}

GsDecodeBatch signature_batch(std::uint32_t sequence, std::uint32_t prim, std::uint64_t tex0, std::uint64_t tex1,
                              std::uint64_t frame, std::uint64_t alpha, std::uint64_t clamp) {
  GsDecodeBatch batch;
  batch.vblank = 101;
  batch.sequence = sequence;
  batch.path = 2;
  batch.vertices = 40;
  batch.drawing_vertices = 20;
  batch.full_screen = true;
  batch.min_x = -0.5f;
  batch.max_x = 639.5f;
  batch.min_y = -0.5f;
  batch.max_y = 447.5f;
  batch.state.prim = prim;
  batch.state.tex0 = tex0;
  batch.state.tex1 = tex1;
  batch.state.frame = frame;
  batch.state.alpha = alpha;
  batch.state.clamp = clamp;
  batch.state.xyoffset = 0x0000720800006c08ull;
  return batch;
}

void test_stream_census(const std::string &dir) {
  const std::string path = dir + "/census.gt4gs";
  std::vector<std::vector<std::uint8_t>> records;
  {
    // XYOFFSET_1 -> main view.
    std::vector<std::uint8_t> record;
    put_tag(record, 1, true, false, 0, 0, 1, 0xE);
    put_ad(record, 0x18, 0x0000720800006c08ull);
    records.push_back(record);
  }
  {
    // PRIM sprite (TME + ABE) and RGBA (0,0,0,128).
    std::vector<std::uint8_t> record;
    put_tag(record, 2, true, false, 0, 0, 1, 0xE);
    put_ad(record, 0x00, 0x15E);
    put_ad(record, 0x01, 0x3F80000080000000ull);
    records.push_back(record);
  }
  {
    // REGLIST sprite: [UV, XYZF3, UV, XYZF2], one sprite from (-0.5,-0.5) to (7.5,7.5).
    std::vector<std::uint8_t> record;
    put_tag(record, 1, true, false, 0, 1, 4, 0x43C3);  // descriptors 3, 12, 3, 4
    put(record, 0x0000000000000000ull, 0x000000FF72006C00ull);  // UV 0,0 then XYZF3 (0,0)
    put(record, 0x00000700000007D0ull, 0x000000FF72806C80ull);  // UV 2000,1792 then XYZF2 (8,8)
    records.push_back(record);
  }
  {
    // IMAGE upload: BITBLTBUF/TRXPOS/TRXREG/TRXDIR, then the IMAGE tag alone (its data is the next record).
    std::vector<std::uint8_t> record;
    put_tag(record, 4, true, false, 0, 0, 1, 0xE);
    put_ad(record, 0x50, 0x0000000000000000ull);                 // SBP 0, DBP 0
    put_ad(record, 0x51, 0x0000000000000000ull);                 // TRXPOS
    put_ad(record, 0x52, 0x00040004ull);                         // RRW 4, RRH 4
    put_ad(record, 0x53, 0x0000000000000000ull);                 // TRXDIR host->local
    put_tag(record, 4, true, false, 0, 2, 16, 0);                // IMAGE, 4 qwords
    records.push_back(record);
  }
  {
    // The four IMAGE data qwords, in their own capture record (the S1 split).
    std::vector<std::uint8_t> record;
    for (int index = 0; index < 4; ++index) put(record, 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull);
    records.push_back(record);
  }
  write_file(path, records, 100);
  GsStreamCensus census;
  std::vector<GsDecodeBatch> batches;
  std::vector<GsDecodeImageUpload> uploads;
  if (!load_and_census(path, census, batches, uploads)) return;
  check(census.records == 7, "five GIF records plus the frame markers");
  check(census.gif_records[2] == 5, "five PATH2 records");
  check(census.packets == 5, "five GIF packets (four state packets and the IMAGE tag)");
  check(census.image_uploads == 1, "one IMAGE upload");
  check(census.image_bytes == 64, "the IMAGE upload carries 4 qwords");
  check(census.split_packets == 1, "the IMAGE packet is split across two capture records");
  check(batches.size() == 1, "one primitive batch");
  if (!batches.empty()) {
    check(batches[0].vertices == 2 && batches[0].drawing_vertices == 1, "sprite has a latch and a draw vertex");
    check(std::fabs(batches[0].min_x - -0.5f) < 1e-4f && std::fabs(batches[0].max_x - 7.5f) < 1e-4f,
          "sprite x maps through XYOFFSET");
    check(std::fabs(batches[0].min_y - -0.5f) < 1e-4f && std::fabs(batches[0].max_y - 7.5f) < 1e-4f,
          "sprite y maps through XYOFFSET");
    check(!batches[0].full_screen, "a small sprite is not full screen");
    check(batches[0].state.prim_value() == 0x15E, "the PRIM in force is the sprite state");
  }
  const GsFrameCensus &row = census.frames[100];
  check(row.capture_vblank == 100 && row.scene_frame == 99, "the V -> V-1 attribution");
  check(row.ad_writes == 7, "seven A+D writes");
  check(row.image_uploads == 1 && row.image_bytes == 64, "the per-frame image census");
}

void test_full_screen(const std::string &dir) {
  const std::string path = dir + "/full.gt4gs";
  std::vector<std::vector<std::uint8_t>> records;
  {
    std::vector<std::uint8_t> record;
    put_tag(record, 2, true, false, 0, 0, 1, 0xE);
    put_ad(record, 0x18, 0x0000720800006c08ull);
    put_ad(record, 0x00, 0x15E);
    records.push_back(record);
  }
  {
    // One REGLIST sprite covering 0..640 x 0..448 (the screen mapping is relative to XYOFFSET).
    std::vector<std::uint8_t> record;
    put_tag(record, 1, true, false, 0, 1, 4, 0x43C3);
    put(record, 0, 0x000000FF72086C08ull);                       // UV 0,0 then XYZF3 at (0,0)
    put(record, 0x00008E0000028000ull, 0x000000FF8E089408ull);   // UV then XYZF2 at (640,448)
    records.push_back(record);
  }
  write_file(path, records, 100);
  GsStreamCensus census;
  std::vector<GsDecodeBatch> batches;
  std::vector<GsDecodeImageUpload> uploads;
  if (!load_and_census(path, census, batches, uploads)) return;
  check(census.full_screen_batches == 1, "one full-screen batch");
  if (!batches.empty()) check(batches[0].full_screen, "the 640 px strip batch is full screen");
}

void test_wet_composite_recognition() {
  std::vector<GsDecodeBatch> batches;
  GsDecodeBatch clear = signature_batch(10, 0x006, 0x1A40ull, 0, 0x00FFFFFF000A00D2ull, 0x0000008000000044ull,
                                        0x000001FC001FC00Aull);
  clear.state.rgba = 0;  // RGBA (0,0,0,0): the alpha clear writes 0
  batches.push_back(clear);
  const std::uint64_t composite_tex0 =
      0x3140ull | (2ull << 14) | (7ull << 26) | (7ull << 30) | (1ull << 34);  // TBP0, TBW 2, TW/TH 7, TCC 1
  GsDecodeBatch composite =
      signature_batch(11, 0x15E, composite_tex0, 0x60ull, 0x00000000000A00D2ull, 0x58ull, 0x000001FC001FC00Aull);
  composite.state.alpha = 0x58;                  // (Cs - 0) * Ad + Cd
  composite.state.zbuf = 0x0000000101000000ull;  // ZMSK 1
  batches.push_back(composite);
  const std::uint64_t multiply_selectors = 1u | (2u << 2) | (2u << 4) | (2u << 6);
  GsDecodeBatch darken =
      signature_batch(12, 0x04E, 0, 0, 0x00000000000A00D2ull, (0x7Full << 32) | multiply_selectors,
                      0x000001FC001FC00Aull);
  batches.push_back(darken);
  GsDecodeBatch brighten =
      signature_batch(13, 0x04E, 0, 0, 0x00000000000A00D2ull, (0xFFull << 32) | multiply_selectors,
                      0x000001FC001FC00Aull);
  batches.push_back(brighten);
  GsWetCompositeInfo info;
  check(find_wet_composite(batches, 100, info), "the measured four-draw chain is recognised");
  check(info.found && info.alpha_clear == &batches[0] && info.composite == &batches[1] &&
            info.post_darken == &batches[2] && info.post_brighten == &batches[3],
        "the recogniser pins the exact batches");
  check(info.stream_vblank == 101 && info.scene_vblank == 100, "the composite uses the V+1 stream");
  // Disturb the composite's blend: the recogniser must refuse, not fall back to another draw.
  GsDecodeBatch wrong = composite;
  wrong.state.alpha = 0x44;  // normal alpha blend, not Cs*Ad+Cd
  std::vector<GsDecodeBatch> broken{clear, wrong, darken, brighten};
  GsWetCompositeInfo refused;
  check(!find_wet_composite(broken, 100, refused), "a wrong blend refuses the chain");
  check(!refused.refusal.empty(), "the refusal carries a named reason");
}

// ---- TASK-090 post chain: the DTHE/DIMX/TEX2 decode, the per-vertex RGBA and the full-chain census -----------------

// Sets up the exact measured state of one chain op. The values are the ones the night capture carries (S2 census and
// the post-chain study); each call pins one signature.
GsDecodeBatch chain_batch(GsChainOp op, std::uint32_t sequence) {
  GsDecodeBatch batch;
  batch.vblank = 101;
  batch.sequence = sequence;
  batch.path = 2;
  batch.vertices = 20;
  batch.drawing_vertices = 10;
  batch.full_screen = true;
  batch.min_x = -0.5f;
  batch.max_x = 639.5f;
  batch.min_y = -0.5f;
  batch.max_y = 447.5f;
  GsDecodeState &state = batch.state;
  state.xyoffset = 0x0000720800006C08ull;
  state.zbuf = 0x0000000001000046ull;  // ZMSK 0: the frame clears and the frame sample write depth
  state.alpha = 0x0000008000000044ull;
  state.colclamp = 1;
  switch (op) {
  case GsChainOp::pre_world_clear_rgb:
    state.prim = 0x006;
    state.frame = 0x00000000000A00D2ull;
    state.test = 0x30000;
    state.rgba = 0x3F80000000000000ull;
    break;
  case GsChainOp::pre_world_clear_alpha:
    state.prim = 0x006;
    state.frame = 0x00FFFFFF000A00D2ull;
    state.test = 0x30000;
    state.rgba = 0x3F80000000000000ull;
    break;
  case GsChainOp::pre_world_clear_test:
    state.prim = 0x006;
    state.frame = 0x00000000000A00D2ull;
    state.test = 0x32001;
    state.rgba = 0x3F80000000000000ull;
    break;
  case GsChainOp::frame_sample:
    state.prim = 0x15E;
    state.frame = 0xFFFFFFFF000A00D2ull;
    state.test = 0x3800F;
    state.rgba = 0x3F80000080004D80ull;  // (128,77,0,128)
    state.tex0 = 0x0006300668029A40ull;  // TBP0 0x1A40, CT32, TW 10, TH 9, TCC 1
    state.tex1 = 0;
    state.clamp = 0x000006FC009FC00Aull;  // region 0..639 x 0..447
    state.zbuf = 0x0000000001000046ull;   // ZMSK 0: the draw writes z = 1 (the depth clear)
    break;
  case GsChainOp::reflection_alpha_clear:
    state.prim = 0x006;
    state.frame = 0x00FFFFFF000A00D2ull;
    state.test = 0;
    state.rgba = 0x3F80000000000000ull;
    break;
  case GsChainOp::wet_composite:
    state.prim = 0x15E;
    state.frame = 0x00000000000A00D2ull;
    state.test = 0x3820C;
    state.rgba = 0x3F80000040404040ull;  // (64,64,64,128)
    state.tex0 = 0x3140ull | (2ull << 14) | (7ull << 26) | (7ull << 30) | (1ull << 34);
    state.tex1 = 0x60;
    state.clamp = 0x000001FC001FC00Aull;
    state.alpha = 0x58;
    state.zbuf = 0x0000000101000046ull;  // ZMSK 1
    break;
  case GsChainOp::post_multiply:
    state.prim = 0x04E;
    state.frame = 0x00000000000A00D2ull;
    state.test = 0x3820C;
    state.alpha = (0x7Full << 32) | 0xA9ull;
    break;
  case GsChainOp::present_ct16:
    state.prim = 0x11E;
    state.frame = 0x00000000020A008Cull;  // FBP 140, FBW 10, PSM 2 (CT16)
    state.test = 0x3820C;
    state.rgba = 0x3F80000080808080ull;  // (128,128,128,128)
    state.tex0 = 0x0007140668029A40ull;
    state.tex1 = 0;
    state.clamp = 0x000006FC009FC00Aull;
    state.dthe = 1;
    state.dimx = 0x76543210ABCDEF01ull;
    state.zbuf = 0x0000000101000046ull;  // ZMSK 1
    break;
  case GsChainOp::display_alpha_clear:
    state.prim = 0x006;
    state.frame = 0x00FFFFFF020A008Cull;
    state.test = 0;
    state.rgba = 0x3F80000000000000ull;
    break;
  default:
    break;
  }
  return batch;
}

void test_post_chain_recognition() {
  std::vector<GsDecodeBatch> batches;
  batches.push_back(chain_batch(GsChainOp::pre_world_clear_rgb, 672));
  batches.push_back(chain_batch(GsChainOp::pre_world_clear_alpha, 675));
  batches.push_back(chain_batch(GsChainOp::pre_world_clear_test, 678));
  batches.push_back(chain_batch(GsChainOp::pre_world_clear_test, 681));
  batches.push_back(chain_batch(GsChainOp::frame_sample, 684));
  batches.push_back(chain_batch(GsChainOp::reflection_alpha_clear, 920));
  batches.push_back(chain_batch(GsChainOp::wet_composite, 934));
  GsDecodeBatch darken = chain_batch(GsChainOp::post_multiply, 936);
  darken.state.alpha = (0x7Full << 32) | 0xA9ull;
  batches.push_back(darken);
  GsDecodeBatch brighten = chain_batch(GsChainOp::post_multiply, 964);
  brighten.state.alpha = (0xFFull << 32) | 0xA9ull;
  batches.push_back(brighten);
  batches.push_back(chain_batch(GsChainOp::present_ct16, 1094));
  batches.push_back(chain_batch(GsChainOp::display_alpha_clear, 1207));
  // One unrelated full-screen batch: counted as unknown, excluded, and it must not refuse the chain.
  GsDecodeBatch odd = chain_batch(GsChainOp::pre_world_clear_rgb, 700);
  odd.state.test = 0x1234;
  odd.sequence = 1210;
  batches.push_back(odd);
  GsPostChainInfo info;
  const bool chain_found = find_post_chain(batches, 100, info);
  check(chain_found, "the eleven-batch chain is recognised");
  check(info.found && info.stream_vblank == 101 && info.scene_vblank == 100, "the chain uses the V+1 stream");
  check(info.full_screen_batches == 12, "every full-screen batch is counted");
  check(info.counts[static_cast<std::size_t>(GsChainOp::pre_world_clear_rgb)] == 1, "one RGB frame clear");
  check(info.counts[static_cast<std::size_t>(GsChainOp::pre_world_clear_alpha)] == 1, "one alpha frame clear");
  check(info.counts[static_cast<std::size_t>(GsChainOp::pre_world_clear_test)] == 2, "two TEST clears");
  check(info.counts[static_cast<std::size_t>(GsChainOp::frame_sample)] == 1, "one frame sample");
  check(info.counts[static_cast<std::size_t>(GsChainOp::reflection_alpha_clear)] == 1, "one reflection alpha clear");
  check(info.counts[static_cast<std::size_t>(GsChainOp::wet_composite)] == 1, "one wet composite");
  check(info.counts[static_cast<std::size_t>(GsChainOp::post_multiply)] == 2, "two multiplies");
  check(info.counts[static_cast<std::size_t>(GsChainOp::present_ct16)] == 1, "one CT16 present");
  check(info.counts[static_cast<std::size_t>(GsChainOp::display_alpha_clear)] == 1, "one display alpha clear");
  check(info.unrecognised_full_screen == 1 &&
            info.counts[static_cast<std::size_t>(GsChainOp::unknown)] == 1,
        "the unrelated full-screen batch is counted as unknown");
  check(info.alpha_clear == &batches[5] && info.composite == &batches[6] && info.post_darken == &batches[7] &&
            info.post_brighten == &batches[8] && info.present == &batches[9],
        "the recogniser pins the five chain draws");
  check(!info.entries[4].after_world && info.entries[4].op == GsChainOp::frame_sample,
        "the frame sample sits before the world draws");
  check(info.entries[5].after_world == false && info.entries[5].op == GsChainOp::reflection_alpha_clear,
        "the pre-reflection clear is applied inside the world render");
  check(info.entries[6].after_world && info.entries[7].after_world && info.entries[8].after_world &&
            info.entries[9].after_world,
        "the composite, multiplies and present execute over the world image");
  // The present's captured dither state is decoded, not guessed.
  check(batches[9].state.dthe_on() && batches[9].state.dimx == 0x76543210ABCDEF01ull && batches[9].state.colclamp == 1,
        "the present carries DTHE, DIMX and COLCLAMP");
  // Missing present: a named refusal.
  std::vector<GsDecodeBatch> no_present(batches.begin(), batches.begin() + 9);
  GsPostChainInfo refused;
  check(!find_post_chain(no_present, 100, refused), "a chain without the present is refused");
  check(!refused.refusal.empty(), "the present refusal carries a named reason");
  // Wrong order: the present before the multiplies.
  std::vector<GsDecodeBatch> reordered;
  reordered.push_back(batches[0]);
  reordered.push_back(batches[5]);
  reordered.push_back(batches[6]);
  reordered.push_back(batches[9]);
  reordered.push_back(batches[7]);
  reordered.push_back(batches[8]);
  GsPostChainInfo order_refused;
  check(!find_post_chain(reordered, 100, order_refused), "a chain out of the measured order is refused");
}

void test_dither_and_vertex_colour_decode(const std::string &dir) {
  const std::string path = dir + "/dither.gt4gs";
  std::vector<std::vector<std::uint8_t>> records;
  {
    // A+D DTHE/DIMX/TEX2/COLCLAMP writes.
    std::vector<std::uint8_t> record;
    put_tag(record, 4, true, false, 0, 0, 1, 0xE);
    put_ad(record, 0x45, 0x1);
    put_ad(record, 0x44, 0x0123456789ABCDEFull);
    put_ad(record, 0x16, 0x0000000000001234ull);  // TEX2: CBP 0x1234, CLD 0
    put_ad(record, 0x46, 0x1);
    records.push_back(record);
  }
  {
    // REGLIST sprite [RGBAQ, XYZF3, RGBAQ, XYZF2]: the two kicks carry different colours.
    std::vector<std::uint8_t> record;
    put_tag(record, 1, true, false, 0, 1, 4, 0x41C1);  // descriptors 1, 12, 1, 4
    put(record, 0x3F800000801E140Aull, 0x0000000000000000ull);  // RGBA (10,20,30,128), Q; XYZF3 (0,0)
    put(record, 0x3F800000403C3228ull, 0x0000000000000000ull);  // RGBA (40,50,60,64), Q; XYZF2 (0,0)
    records.push_back(record);
  }
  write_file(path, records, 100);
  GsStreamCensus census;
  std::vector<GsDecodeBatch> batches;
  std::vector<GsDecodeImageUpload> uploads;
  if (!load_and_census(path, census, batches, uploads)) return;
  check(batches.size() == 1, "one sprite batch");
  if (!batches.empty()) {
    const GsDecodeBatch &batch = batches[0];
    check(batch.state.dthe_on(), "DTHE bit 0 is decoded from the stream");
    check(batch.state.dimx == 0x0123456789ABCDEFull, "DIMX is decoded from the stream");
    check(batch.state.tex2_cbp() == 0x1234, "TEX2 CBP is decoded from the stream");
    check(batch.state.colclamp == 1, "COLCLAMP is decoded from the stream");
    check(batch.verts.size() == 2, "two kicked vertices");
    check(batch.verts[0].rgba == 0x801E140Aull, "the latch vertex carries its own RGBAQ (10,20,30,128)");
    check(batch.verts[1].rgba == 0x403C3228ull, "the draw vertex carries its own RGBAQ (40,50,60,64)");
  }
  // DTHE bit 0 decides, not bit 1.
  GsDecodeState state;
  state.dthe = 0x2;
  check(!state.dthe_on(), "DTHE bit 0 decides the dither, not bit 1");
  state.dthe = 1;
  check(state.dthe_on(), "DTHE bit 0 enables the dither");
}

}  // namespace

int main(int argc, char **argv) {
  const std::string dir = argc > 1 ? argv[1] : "coregs3_gsdecode_work";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  test_stream_census(dir);
  test_full_screen(dir);
  test_wet_composite_recognition();
  test_post_chain_recognition();
  test_dither_and_vertex_colour_decode(dir);
  if (failures != 0) {
    std::fprintf(stderr, "coregs3_gsdecode: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("coregs3_gsdecode: all checks passed\n");
  return 0;
}
