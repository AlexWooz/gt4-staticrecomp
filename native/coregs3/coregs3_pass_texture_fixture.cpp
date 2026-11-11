// CTest native_coregs3_pass_textures: TASK-090 pass textures, host only, hand-authored data (no game data).
//
// It pins the two rules this round adds:
//   * the `.gt4vram` version 2 record format: the explicit record kind and PassId, the backwards-compatible version 1
//     (legacy) header, the self-describing advance over mixed records, and every malformed variant (unknown version,
//     bad kind, pass out of range, nonzero reserved, truncated header/bytes);
//   * the per-pass SLM binding of the offline texture table: two world-pass items name the same 40-byte texture record,
//     but each decodes its pixels against the image captured at its own pass begin, so their decoded RGBA8 differs, and
//     the payload cache never shares a decode across images. A pass with no image falls back to the default (frame-end)
//     image and is counted.
#include "coregs3_tap.hpp"
#include "coregs3_texture.hpp"
#include "coregs3_texture_frame.hpp"
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

// ---- The record format ----

std::vector<std::uint8_t> sidecar_record(std::uint32_t kind, std::uint32_t pass, std::uint64_t vblank,
                                         std::uint32_t first_block, std::uint32_t block_count,
                                         std::uint8_t fill) {
  vram::Header header;
  header.vblank = vblank;
  header.first_block = first_block;
  header.block_count = block_count;
  header.kind = kind;
  header.pass = pass;
  header.window_count = 1;
  std::vector<std::uint8_t> out(vram::kHeaderBytes + vram::kWindowBytes +
                                    static_cast<std::size_t>(block_count) * vram::kBlockBytes,
                                fill);
  vram::encode_header(out.data(), header);
  vram::encode_window(out.data() + vram::kHeaderBytes, vram::Window{first_block, block_count});
  return out;
}

std::vector<std::uint8_t> legacy_record(std::uint64_t vblank, std::uint32_t first_block, std::uint32_t block_count,
                                        std::uint8_t fill) {
  vram::Header header;
  header.vblank = vblank;
  header.first_block = first_block;
  header.block_count = block_count;
  std::vector<std::uint8_t> out(32u + static_cast<std::size_t>(block_count) * vram::kBlockBytes, fill);
  vram::encode_legacy_header(out.data(), header);
  std::fill(out.begin() + 32, out.end(), fill);
  return out;
}

void check_record_format() {
  // A version 2 pass-begin record round-trips kind, pass, vblank and its bytes.
  std::vector<std::uint8_t> file = sidecar_record(vram::kRecordPassBegin, kPassCourse, 7398, 0x36D0, 2, 0xA5);
  std::vector<VramRecord> records;
  std::string error;
  require(parse_vram_sidecar(file.data(), file.size(), records, error), "v2 pass-begin record parses");
  require(records.size() == 1, "one record");
  require(records[0].kind == vram::kRecordPassBegin, "record kind is pass-begin");
  require(records[0].pass == kPassCourse, "record pass is the course");
  require(records[0].vblank == 7398, "record vblank is the capture frame's own");
  require(records[0].windows.size() == 1 && records[0].windows[0].first_block == 0x36D0 &&
              records[0].windows[0].block_count == 2,
          "record block range");
  require(records[0].bytes.size() == 2u * vram::kBlockBytes && records[0].bytes[0] == 0xA5, "record bytes");

  // A version 1 file still parses and becomes a vblank record with pass 0.
  file = legacy_record(7399, 0x2BC0, 3, 0x5A);
  require(parse_vram_sidecar(file.data(), file.size(), records, error), "legacy v1 record parses");
  require(records.size() == 1, "one legacy record");
  require(records[0].kind == vram::kRecordVblank && records[0].pass == 0, "legacy record is a vblank record");
  require(records[0].vblank == 7399 && records[0].bytes[0] == 0x5A, "legacy record fields");
  require(records[0].windows.size() == 1 && records[0].windows[0].first_block == 0x2BC0 &&
              records[0].windows[0].block_count == 3,
          "legacy record window");

  // Mixed records advance by their own header length (32 for v1, 40 for v2).
  std::vector<std::uint8_t> mixed = legacy_record(7399, 0x2BC0, 1, 0x11);
  const std::vector<std::uint8_t> mark = sidecar_record(vram::kRecordPassBegin, kPassSky, 7398, 0x2BC0, 1, 0x22);
  mixed.insert(mixed.end(), mark.begin(), mark.end());
  require(parse_vram_sidecar(mixed.data(), mixed.size(), records, error), "mixed v1/v2 file parses");
  require(records.size() == 2, "mixed file record count");
  require(records[1].kind == vram::kRecordPassBegin && records[1].pass == kPassSky, "mixed second record");
  require(records[1].bytes[0] == 0x22, "mixed second record bytes");

  auto expect_bad = [](std::vector<std::uint8_t> bytes, const char *label) {
    std::vector<VramRecord> parsed;
    std::string reason;
    require(!parse_vram_sidecar(bytes.data(), bytes.size(), parsed, reason), label);
    require(!reason.empty(), label);
  };
  std::vector<std::uint8_t> bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad[4] = 3;  // version 3 is not a format this decoder knows
  expect_bad(bad, "unknown version refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad[28] = 4;  // kind 4
  expect_bad(bad, "unknown record kind refused");
  bad = sidecar_record(vram::kRecordPassBegin, 200, 7400, 0x2BC0, 1, 0);
  expect_bad(bad, "pass id outside the bound refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad[36] = 1;  // a label on a non-draw record
  expect_bad(bad, "label on a vblank record refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad[40] = 0;  // window_count 0
  expect_bad(bad, "empty window table refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad[44] = 1;  // the version 2 reserved word
  expect_bad(bad, "nonzero version 2 reserved refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad.resize(vram::kHeaderBytes);
  expect_bad(bad, "truncated window table refused");
  bad = sidecar_record(vram::kRecordVblank, 0, 7400, 0x2BC0, 1, 0);
  bad.resize(vram::kHeaderBytes + vram::kWindowBytes + 100);
  expect_bad(bad, "truncated version 2 bytes refused");

  // The snapshot channel: with no sink installed the calls are no-ops; a sink receives every request once, with the
  // pass marker and the draw windows intact.
  vram::set_snapshot_sink(nullptr);
  require(!vram::snapshot_sink_installed(), "no sink by default");
  vram::reset_snapshot_counters();
  vram::note_pass_begin(kPassCourse, 7398);
  require(vram::snapshot_counters().requests == 0, "no request without a sink");
  static vram::SnapshotRequest seen{};
  static int calls = 0;
  vram::set_snapshot_sink([](const vram::SnapshotRequest &request) noexcept {
    seen = request;
    ++calls;
  });
  require(vram::snapshot_sink_installed(), "sink installed");
  vram::note_pass_begin(kPassCourse, 7398);
  require(calls == 1 && seen.kind == vram::kRecordPassBegin && seen.pass == kPassCourse && seen.vblank == 7398,
          "sink receives the pass marker");
  const vram::Window windows[2] = {{0x36D0u, 32u}, {0x36D2u, 4u}};
  vram::note_draw_textures(42, 7398, windows, 2);
  require(calls == 2 && seen.kind == vram::kRecordDrawTexture && seen.label == 42 && seen.window_count == 2 &&
              seen.windows[0].first_block == 0x36D0u && seen.windows[1].block_count == 4u,
          "sink receives the draw windows");
  require(vram::snapshot_counters().requests == 2 && vram::snapshot_counters().delivered == 2, "channel counters");
  vram::set_snapshot_sink(nullptr);

  // A job record (kind 3) round-trips its label and windows; the label is what ties it to the tap's job.
  std::vector<std::uint8_t> jobBytes(vram::kHeaderBytes + vram::kWindowBytes + 2u * vram::kBlockBytes, 0x33);
  vram::Header jobHeader;
  jobHeader.vblank = 7399;
  jobHeader.first_block = 0x36D0;
  jobHeader.block_count = 2;
  jobHeader.kind = vram::kRecordJobTexture;
  jobHeader.label = 460177;
  jobHeader.window_count = 1;
  vram::encode_header(jobBytes.data(), jobHeader);
  vram::encode_window(jobBytes.data() + vram::kHeaderBytes, vram::Window{0x36D0u, 2u});
  std::vector<VramRecord> jobRecords;
  require(parse_vram_sidecar(jobBytes.data(), jobBytes.size(), jobRecords, error), "job record parses");
  require(jobRecords.size() == 1 && jobRecords[0].kind == vram::kRecordJobTexture &&
              jobRecords[0].label == 460177 && jobRecords[0].windows[0].first_block == 0x36D0u,
          "job record kind and label");
}

// ---- The per-pass texture binding ----

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

std::uint32_t unpack(std::uint32_t vn, std::uint32_t vl, std::uint32_t num, std::uint32_t qword, bool tops,
                     bool usn) {
  return 0x60000000u | ((vn & 3u) << 26) | ((vl & 3u) << 24) | ((num & 0xFFu) << 16) | (tops ? 0x8000u : 0u) |
         (usn ? 0x4000u : 0u) | (qword & 0x3FFu);
}

void put_stmod(std::vector<std::uint8_t> &packet, std::uint32_t mode) { put_u32(packet, 0x05000000u | (mode & 3u)); }
void put_strow(std::vector<std::uint8_t> &packet, std::uint32_t x, std::uint32_t y, std::uint32_t z,
               std::uint32_t w) {
  put_u32(packet, 0x30000000u);
  put_u32(packet, x);
  put_u32(packet, y);
  put_u32(packet, z);
  put_u32(packet, w);
}
void put_stmask(std::vector<std::uint8_t> &packet, std::uint32_t mask) {
  put_u32(packet, 0x20000000u);
  put_u32(packet, mask);
}

// A four-vertex world packet: the TOPS 0xC0 descriptor (0x8000 | count), the MODE 2 delta chain, four V2-16 UVs, a
// V4-8 colour and the masked strip table. The shape decoder's world rule accepts it.
std::vector<std::uint8_t> make_world_packet(std::uint32_t count) {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | count);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_stmod(packet, 2);
  put_strow(packet, 0x100u, 0x200u, 0x300u, 0);
  put_u32(packet, unpack(2, 2, 1, 0x00, true, true));  // V3-8 seed commit
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u32(packet, unpack(2, 1, 2, 0x01, true, false));  // V3-16 delta x2
  put_u16(packet, 10);
  put_u16(packet, 20);
  put_u16(packet, 30);
  put_u16(packet, 1);
  put_u16(packet, 2);
  put_u16(packet, 3);
  put_u32(packet, unpack(2, 0, 1, 0x03, true, false));  // V3-32 delta x1
  put_u32(packet, 1);
  put_u32(packet, 2);
  put_u32(packet, 3);
  put_u32(packet, 0);
  put_stmod(packet, 0);
  put_u32(packet, unpack(1, 1, 4, 0x40, true, false));  // V2-16 UV x4
  put_u16(packet, 0x1000);
  put_u16(packet, 0x2000);
  put_u16(packet, 0x0800);
  put_u16(packet, 0x0000);
  put_u16(packet, 0xF000);
  put_u16(packet, 0x0FFF);
  put_u16(packet, 0x0000);
  put_u16(packet, 0x0000);
  put_u32(packet, unpack(3, 2, 4, 0x80, true, true));  // V4-8 colour x4
  for (std::uint8_t value = 10; value <= 160; value = static_cast<std::uint8_t>(value + 10)) put_u8(packet, value);
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));  // masked V1-8 strip table x2
  put_u8(packet, 1);
  put_u8(packet, 6);
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

// The 40-byte PGLUtexture record of a T4 32x16 texture at 0x36D0 with a CSM1 palette at 0x36D2 (CLD = 1, the record
// path this fixture exercises; the world stream's CLD = 0/TEX2 form is the state fixture's subject).
std::array<std::uint8_t, 40> make_t4_record() {
  const std::uint64_t tex0 = 0x36D0ull | (1ull << 14) | (0x14ull << 20) | (5ull << 26) | (4ull << 30) |
                             (1ull << 34) | (0ull << 35) | (0x36D2ull << 37) | (0ull << 51) | (0ull << 55) |
                             (0ull << 56) | (1ull << 61);
  const std::uint64_t tex1 = 0ull | (0ull << 2) | (1ull << 5) | (1ull << 6);
  const std::uint64_t clamp = 0ull;  // WMS/WMT = REPEAT: the full 32x16 texture, no region
  std::array<std::uint8_t, 40> bytes{};
  for (int index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((tex0 >> (8 * index)) & 0xFFu);
    bytes[8 + index] = static_cast<std::uint8_t>((tex1 >> (8 * index)) & 0xFFu);
    bytes[16 + index] = static_cast<std::uint8_t>((0ull >> (8 * index)) & 0xFFu);
    bytes[24 + index] = static_cast<std::uint8_t>((0ull >> (8 * index)) & 0xFFu);
    bytes[32 + index] = static_cast<std::uint8_t>((clamp >> (8 * index)) & 0xFFu);
  }
  return bytes;
}

void put_u32_at(std::uint8_t *out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
}

// A 4 MiB image whose palette carries a distinguishable word at CSM1 entry 0. The T4 texels stay zero, so every texel
// samples index 0 and the decoded RGBA is that palette word's four bytes (R in the low byte, the GS CSM1 CT32 order).
SlmImage make_image(std::uint32_t palette) {
  SlmImage image;
  image.cover(vram::Window{0u, vram::kBlocksTotal});
  std::fill(image.local.bytes().begin(), image.local.bytes().end(), 0u);
  for (std::uint32_t block = 0; block < 4u; ++block)
    put_u32_at(image.local.bytes().data() + (0x36D2u + block) * vram::kBlockBytes, palette);
  return image;
}

void check_pass_binding() {
  FrameSnapshot frame;
  frame.vblank = 7398;
  CapturedTexture record{};
  record.address = 0x01AB0000u;
  const std::array<std::uint8_t, 40> bytes = make_t4_record();
  std::memcpy(record.bytes, bytes.data(), bytes.size());
  frame.textures.push_back(record);

  const std::vector<std::uint8_t> packet = make_world_packet(4);
  frame.packets = packet;
  for (std::uint32_t pass : {static_cast<std::uint32_t>(kPassSky), static_cast<std::uint32_t>(kPassCourse)}) {
    CapturedDraw draw;
    draw.sequence = pass;
    draw.pass = pass;
    draw.item_count = 1;
    draw.items[0].packet_offset = 0;
    draw.items[0].packet_bytes = static_cast<std::uint32_t>(packet.size());
    draw.items[0].qwords = static_cast<std::uint16_t>((packet.size() + 15u) / 16u);
    draw.items[0].texture_record = 0;
    frame.draws.push_back(draw);
  }

  // The default (frame-end) image and the sky pass's own captured image differ in texels and palette.
  SlmImage default_image = make_image(0x11223344u);
  SlmImage sky_image = make_image(0x55667788u);
  PassSlmImages pass_images;
  pass_images.images[kPassSky] = &sky_image;

  TextureFrameOptions options;
  options.use_tap = false;
  options.world_channels = true;
  options.pass_slms = &pass_images;
  FrameTextureTable table;
  TextureFrameCounters counters;
  std::string error;
  require(build_frame_texture_table(frame, default_image, nullptr, {}, options, table, counters, error),
          "the pass-image table builds");
  require(table.draws.size() == 2 && table.draws[0].size() == 1 && table.draws[1].size() == 1, "two one-item draws");
  const SampledTexture *sky = table.draws[0][0].texture;
  const SampledTexture *course = table.draws[1][0].texture;
  require(sky != nullptr && course != nullptr, "both world items are textured");
  require(sky != course, "the two passes did not share one sampler");
  require(sky->from_state == false && course->from_state == false, "both binds came from the record path");
  require(sky->rgba[0] == 0x88 && sky->rgba[1] == 0x77 && sky->rgba[2] == 0x66 && sky->rgba[3] == 0x55,
          "the sky item sampled the sky pass's texels");
  require(course->rgba[0] == 0x44 && course->rgba[1] == 0x33 && course->rgba[2] == 0x22 && course->rgba[3] == 0x11,
          "the course item sampled the default image's texels");
  require(counters.world_pass_slm_images == 1, "one pass image supplied");
  require(counters.world_pass_slm == 1, "one item bound on its pass image");
  require(counters.world_pass_slm_missing == 1, "one item fell back to the default image");
  require(counters.world_pass_slm + counters.world_pass_slm_missing == 2, "every world item counted");

  // With both images supplied, each pass binds its own.
  PassSlmImages both;
  both.images[kPassSky] = &sky_image;
  SlmImage course_image = make_image(0x99AABBCCu);
  both.images[kPassCourse] = &course_image;
  options.pass_slms = &both;
  FrameTextureTable both_table;
  TextureFrameCounters both_counters;
  require(build_frame_texture_table(frame, default_image, nullptr, {}, options, both_table, both_counters, error),
          "the two-pass table builds");
  const SampledTexture *sky2 = both_table.draws[0][0].texture;
  const SampledTexture *course2 = both_table.draws[1][0].texture;
  require(sky2 != nullptr && course2 != nullptr && sky2 != course2, "each pass has its own sampler");
  require(course2->rgba[0] == 0xCC && course2->rgba[1] == 0xBB && course2->rgba[2] == 0xAA && course2->rgba[3] == 0x99,
          "the course item sampled its own pass image");
  require(both_counters.world_pass_slm_images == 2, "two pass images supplied");
  require(both_counters.world_pass_slm == 2 && both_counters.world_pass_slm_missing == 0, "both items on pass images");

  // The per-VU1-job record takes precedence: the tap's source record maps the sky item's packet address to a job, and
  // that job's windows carry a distinct image.
  SlmImage job_image = make_image(0xDDEEFF00u);
  VramRecord jobRecord;
  jobRecord.kind = vram::kRecordJobTexture;
  jobRecord.label = 460177;
  jobRecord.windows.push_back(vram::Window{0x36D2u, 4u});
  jobRecord.bytes.assign(4u * vram::kBlockBytes, 0u);
  std::memcpy(jobRecord.bytes.data(), job_image.local.bytes().data() + 0x36D2u * vram::kBlockBytes,
              4u * vram::kBlockBytes);
  std::map<std::uint32_t, const VramRecord *> jobRecordMap;
  jobRecordMap[460177] = &jobRecord;
  JobTextureImages jobImages;
  jobImages.records = &jobRecordMap;
  frame.draws[0].items[0].address = 0x00001000u;
  frame.draws[1].items[0].address = 0x00002000u;
  TapFileRecord source;
  source.kind = kTapKindSource;
  source.job = 460177;
  source.vblank = frame.vblank;
  source.address = 0x00001000u;
  std::uint8_t dummyTap = 0;
  options.use_tap = true;
  options.job_textures = &jobImages;
  FrameTextureTable jobTable;
  TextureFrameCounters jobCounters;
  std::vector<TapFileRecord> tapRecords{source};
  require(build_frame_texture_table(frame, default_image, &dummyTap, tapRecords, options, jobTable, jobCounters,
                                    error),
          "the job-overlay table builds");
  const SampledTexture *jobSky = jobTable.draws[0][0].texture;
  const SampledTexture *jobCourse = jobTable.draws[1][0].texture;
  require(jobSky != nullptr && jobCourse != nullptr && jobSky != jobCourse, "the job overlay is its own sampler");
  require(jobSky->rgba[0] == 0x00 && jobSky->rgba[1] == 0xFF && jobSky->rgba[2] == 0xEE && jobSky->rgba[3] == 0xDD,
          "the sky item sampled its job's palette");
  require(jobCounters.world_job_slm == 1 && jobCounters.world_job_slm_records == 1,
          "one item bound on its job record");
  options.use_tap = false;
  options.job_textures = nullptr;

  // Without pass images the old behaviour is exact: both items share the default image's payload.
  options.pass_slms = nullptr;
  FrameTextureTable plain;
  TextureFrameCounters plain_counters;
  require(build_frame_texture_table(frame, default_image, nullptr, {}, options, plain, plain_counters, error),
          "the default-image table builds");
  require(plain.draws[0][0].texture != nullptr && plain.draws[1][0].texture != nullptr, "both textured on the default");
  require(plain.draws[0][0].texture->rgba[0] == 0x44, "the default image was used");
  require(plain_counters.world_pass_slm == 0 && plain_counters.world_pass_slm_missing == 0,
          "no pass-image counters without pass images");
}


// TASK-090 live world: the same two world items, but the pixels come from the live per-job store (packet address ->
// record, captured before the job executed) and the GS state comes from the frame's pass journal, because the live
// path has no `.gt4tap`. The pixels must be the item's own job's, the journal's TEST/ALPHA/XYOFFSET must reach the
// binding, and every live/missing item must be counted.
void check_live_binding() {
  FrameSnapshot frame;
  frame.vblank = 7398;
  CapturedTexture record{};
  record.address = 0x01AB0000u;
  const std::array<std::uint8_t, 40> bytes = make_t4_record();
  std::memcpy(record.bytes, bytes.data(), bytes.size());
  frame.textures.push_back(record);
  const std::vector<std::uint8_t> packet = make_world_packet(4);
  frame.packets = packet;
  CapturedPassState sky_state;
  sky_state.pass = kPassSky;
  sky_state.vblank = frame.vblank;
  sky_state.xyoffset = 0x0000720800006C08ull;
  sky_state.test = 0x0003820Cull;
  sky_state.alpha = 0x00000044ull;
  sky_state.fogcol = 0x004F4F54ull;
  CapturedPassState course_state;
  course_state.pass = kPassCourse;
  course_state.vblank = frame.vblank;
  course_state.xyoffset = 0x0000720800006C08ull;
  course_state.test = 0x0005800Eull;
  course_state.alpha = 0x00000044ull;
  course_state.fogcol = 0x0027272Aull;
  frame.pass_states.push_back(sky_state);
  frame.pass_states.push_back(course_state);
  for (std::uint32_t pass : {static_cast<std::uint32_t>(kPassSky), static_cast<std::uint32_t>(kPassCourse)}) {
    CapturedDraw draw;
    draw.sequence = pass;
    draw.pass = pass;
    draw.item_count = 1;
    draw.items[0].packet_offset = 0;
    draw.items[0].packet_bytes = static_cast<std::uint32_t>(packet.size());
    draw.items[0].qwords = static_cast<std::uint16_t>((packet.size() + 15u) / 16u);
    draw.items[0].texture_record = 0;
    draw.items[0].address = pass == kPassSky ? 0x00001000u : 0x00002000u;
    frame.draws.push_back(draw);
  }

  // One live record per item, each from its own VU1 job and carrying a distinct palette word.
  VramRecord sky_record;
  sky_record.kind = vram::kRecordJobTexture;
  sky_record.label = 7u;
  sky_record.vblank = 7399;
  sky_record.windows.push_back(vram::Window{0x36D0u, 36u});  // the T4 page run plus the palette bank at 0x36D2
  sky_record.bytes.assign(36u * vram::kBlockBytes, 0u);
  const SlmImage sky_image = make_image(0x55667788u);
  std::memcpy(sky_record.bytes.data(), sky_image.local.bytes().data() + 0x36D0u * vram::kBlockBytes,
              36u * vram::kBlockBytes);
  VramRecord course_record;
  course_record.kind = vram::kRecordJobTexture;
  course_record.label = 8u;
  course_record.vblank = 7399;
  course_record.windows.push_back(vram::Window{0x36D0u, 36u});
  course_record.bytes.assign(36u * vram::kBlockBytes, 0u);
  const SlmImage course_image = make_image(0x11223344u);
  std::memcpy(course_record.bytes.data(), course_image.local.bytes().data() + 0x36D0u * vram::kBlockBytes,
              36u * vram::kBlockBytes);
  std::map<std::uint32_t, const VramRecord *> live_map;
  live_map[0x00001000u] = &sky_record;
  live_map[0x00002000u] = &course_record;

  // The base image is empty on purpose (no pass/frame image in the live path): every pixel must come from a job window.
  SlmImage empty_base;
  TextureFrameOptions options;
  options.use_tap = false;
  options.world_channels = true;
  options.live_job_textures = &live_map;
  FrameTextureTable table;
  TextureFrameCounters counters;
  std::string error;
  require(build_frame_texture_table(frame, empty_base, nullptr, {}, options, table, counters, error),
          "the live table builds");
  const SampledTexture *sky = table.draws[0][0].texture;
  const SampledTexture *course = table.draws[1][0].texture;
  require(sky != nullptr && course != nullptr && sky != course, "each live item decodes its own job's pixels");
  require(sky->rgba[0] == 0x88 && sky->rgba[1] == 0x77 && sky->rgba[2] == 0x66 && sky->rgba[3] == 0x55,
          "the sky item sampled its own job's palette");
  require(course->rgba[0] == 0x44 && course->rgba[1] == 0x33 && course->rgba[2] == 0x22 && course->rgba[3] == 0x11,
          "the course item sampled its own job's palette");
  require(counters.world_live_job == 2 && counters.world_live_job_missing == 0, "both live items counted");
  require(counters.world_pass_journal_state == 2, "both world items took their GS state from the pass journal");
  const SampleState &sky_state_out = table.draws[0][0].state;
  const SampleState &course_state_out = table.draws[1][0].state;
  require(sky_state_out.from_tap && sky_state_out.world_pass_default, "the sky state is journal state");
  require(sky_state_out.test_known && sky_state_out.test == 0x0003820Cull, "the sky journal TEST reached the binding");
  require(sky_state_out.xyoffset_known && sky_state_out.xyoffset == 0x0000720800006C08ull,
          "the sky journal XYOFFSET reached the binding");
  require(course_state_out.test_known && course_state_out.test == 0x0005800Eull,
          "the course journal TEST reached the binding");
  require(course_state_out.fog_colour == 0x0027272Aull, "the course journal FOGCOL reached the binding");
  require(!sky_state_out.prim_known && sky_state_out.prim == 0x3Cu, "the PRIM keeps the measured default");

  // An item whose address has no live record is counted and falls back to the base image (which covers nothing here, so
  // it stays untextured): the honest outcome, never a stale pixel.
  live_map.erase(0x00002000u);
  FrameTextureTable missing_table;
  TextureFrameCounters missing_counters;
  require(build_frame_texture_table(frame, empty_base, nullptr, {}, options, missing_table, missing_counters, error),
          "the missing-record table builds");
  require(missing_counters.world_live_job == 1 && missing_counters.world_live_job_missing == 1,
          "one live item and one missing item are counted");
  require(missing_table.draws[1][0].texture == nullptr, "an item without a live record stays untextured");
}

}  // namespace

int main() {
  try {
    check_record_format();
    check_pass_binding();
    check_live_binding();
    std::printf("coregs3 pass texture fixture: OK\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("coregs3 pass texture fixture: %s\n", error.what());
    return 1;
  }
}
