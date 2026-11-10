// CTest native_coregs3_wet_reflection: TASK-090 wet reflection, host only, hand-authored data (no game data).
//
// It pins the three rules this round adds:
//   * the reflection pass's selection (coregs3_world.hpp): the main-view items of the captured PassId 7 are selected
//     under the measured state rule, and every other outcome is a named exclusion (the mirror-view items belong to the
//     reflection target's own coordinate system; a missing state, position, program or shape is refused);
//   * the reflection items' texture binding through the per-VU1-job `.gt4vram` record and the tapped TEX0/TEX1/CLAMP/
//     TEX2 state (CLD = 0 with the TEX2 group-0 palette load), including the counted refusal when that TEX2 write is
//     absent;
//   * the render integration: a main-view reflection quad with FRAME.FBMSK 0x00FFFFFF writes destination alpha only
//     (RGB byte-identical to the same quad drawn without the mask), writes no depth (ZBUF.ZMSK = 1), and a quad at a
//     larger view depth than the course is counted as depth-rejected.
#include "coregs3_gif.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_tap.hpp"
#include "coregs3_tap_file.hpp"
#include "coregs3_texture_frame.hpp"
#include "coregs3_vram.hpp"
#include "coregs3_world.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}
void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}
void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }
void put_u32_at(std::uint8_t *out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
}
std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

std::uint32_t unpack(std::uint32_t vn, std::uint32_t vl, std::uint32_t num, std::uint32_t qword, bool tops,
                     bool usn) {
  return 0x60000000u | ((vn & 3u) << 26) | ((vl & 3u) << 24) | ((num & 0xFFu) << 16) | (tops ? 0x8000u : 0u) |
         (usn ? 0x4000u : 0u) | (qword & 0x3FFu);
}
void put_stmask(std::vector<std::uint8_t> &packet, std::uint32_t mask) {
  put_u32(packet, 0x20000000u);
  put_u32(packet, mask);
}

// A four-vertex world packet in the reflection pass's measured form: the TOPS 0xC0 descriptor, V3-32 positions at 0x00,
// V2-16 model UVs at 0x40, a V4-8 colour at 0x80 and the masked strip table. A world shape rule accepts it.
std::vector<std::uint8_t> make_world_packet(std::uint32_t count) {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | count);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_u32(packet, unpack(2, 0, 4, 0x00, true, false));  // V3-32 positions x4
  const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
  for (const auto &corner : corners) {
    put_u32(packet, float_bits(4096.0f * corner[0]));
    put_u32(packet, float_bits(4096.0f * corner[1]));
    put_u32(packet, 0u);
  }
  put_u32(packet, unpack(1, 1, 4, 0x40, true, false));  // V2-16 UVs x4, matching the tapped STQ below
  put_u16(packet, 0x0400);
  put_u16(packet, 0x0400);
  put_u16(packet, 0x0C00);
  put_u16(packet, 0x0400);
  put_u16(packet, 0x0C00);
  put_u16(packet, 0x0C00);
  put_u16(packet, 0x0400);
  put_u16(packet, 0x0C00);
  put_u32(packet, unpack(3, 2, 4, 0x80, true, true));  // V4-8 colour x4
  for (int index = 0; index < 4; ++index) {
    put_u8(packet, 120);
    put_u8(packet, 140);
    put_u8(packet, 160);
    put_u8(packet, 255);
  }
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));  // masked V1-8 strip table x2
  put_u8(packet, 1);
  put_u8(packet, 6);
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

CapturedDraw make_draw(std::uint32_t sequence, std::uint32_t pass, const std::vector<std::uint8_t> &packet,
                       std::uint32_t item_address) {
  CapturedDraw draw;
  draw.sequence = sequence;
  draw.pass = pass;
  draw.item_count = 1;
  draw.items[0].address = item_address;
  draw.items[0].packet_offset = 0;
  draw.items[0].packet_bytes = static_cast<std::uint32_t>(packet.size());
  draw.items[0].qwords = static_cast<std::uint16_t>((packet.size() + 15u) / 16u);
  draw.items[0].texture_record = kNoRecord;
  draw.matrix_packet_valid = 1;
  draw.matrix_packet[0][0] = 0x10000008u;
  draw.matrix_packet[1][0] = float_bits(1.0f / 8192.0f);
  draw.matrix_packet[2][1] = float_bits(1.0f / 8192.0f);
  draw.matrix_packet[4][0] = float_bits(-0.5f);
  draw.matrix_packet[4][1] = float_bits(-0.5f);
  draw.matrix_packet[4][3] = float_bits(1.0f);
  draw.pgl[0] = 0x00020000u;  // the upload ran: the captured packet is the matrix source
  draw.pgl[8] = float_bits(0.0f);
  draw.cmd_value = float_bits(0.0f);
  return draw;
}

// The VU1 program selector: the item's run pair names ITOP (run_itop & 0x3FF) * 8.
void set_program(CapturedDraw &draw, std::uint32_t dispatch) {
  draw.items[0].run_itop = 0x04000000u | (dispatch / 8u);
  draw.items[0].run_mscal = 0x14000000u;
  draw.items[0].run_valid = 1;
  draw.program_dispatch = dispatch;
  draw.program_valid = 1;
}

// ---- The selection rule ----

void check_selection() {
  WorldItemInput item;
  item.pass = kPassReflection;
  item.run_valid = true;
  item.dispatch = 0x0080u;
  item.has_position = true;
  item.shape_status = WorldShapeStatus::ok;
  item.state_known = true;
  item.xyoffset_known = true;
  item.xyoffset = kMainViewXyOffset;
  item.test_known = true;
  item.test = 0x0005820Cull;
  require(world_item_exclusion(item) == WorldExclusion::none,
          "the main-view reflection item is selected under the measured state rule");

  WorldItemInput mirror = item;
  mirror.xyoffset = 0x00007C0800007C08ull;
  require(world_item_exclusion(mirror) == WorldExclusion::mirror,
          "the 0x7C08/0x7C08 reflection item (the sphere mapper's own view) is excluded");

  WorldItemInput no_state = item;
  no_state.state_known = false;
  require(world_item_exclusion(no_state) == WorldExclusion::no_state, "a reflection item without a tapped state");
  no_state = item;
  no_state.test = 0;
  require(world_item_exclusion(no_state) == WorldExclusion::no_state, "a reflection item whose TEST is zero");
  WorldItemInput journal_zero = item;
  journal_zero.test_zero = true;
  require(world_item_exclusion(journal_zero) == WorldExclusion::no_state,
          "a zero-TEST item stays excluded after any journal substitution");

  WorldItemInput no_position = item;
  no_position.has_position = false;
  require(world_item_exclusion(no_position) == WorldExclusion::no_position, "a reflection item without a position");
  WorldItemInput other_program = item;
  other_program.dispatch = 0x0250u;
  require(world_item_exclusion(other_program) == WorldExclusion::itop_other,
          "a reflection item outside ITOP 0x0080 is excluded");
  WorldItemInput run_missing = item;
  run_missing.run_valid = false;
  require(world_item_exclusion(run_missing) == WorldExclusion::itop_other,
          "a reflection item without a valid run pair is excluded");
  WorldItemInput shape = item;
  shape.shape_status = WorldShapeStatus::morph_keys_differ;
  require(world_item_exclusion(shape) == WorldExclusion::shape_refused,
          "a reflection item whose shape rule refuses it is excluded");

  WorldItemInput course = item;
  course.pass = kPassCourse;
  require(world_item_exclusion(course) == WorldExclusion::none, "the course rule is unchanged");
  WorldItemInput cars = item;
  cars.pass = kPassCars;
  require(world_item_exclusion(cars) == WorldExclusion::not_world_pass, "another pass is not selected");
}

// ---- The texture binding (per-job record + tapped CLD = 0/TEX2 state) ----

// One PACKED GIF tag with one descriptor per payload qword. `descriptors[index]` names the register of payload
// `index` (an A+D write uses 0xE and encodes its target in the high qword's low 7 bits).
void put_packed_tag(std::vector<std::uint8_t> &gif, std::uint32_t prim, const std::vector<std::uint32_t> &descriptors,
                    const std::vector<std::array<std::uint64_t, 2>> &payloads) {
  std::uint64_t lo = static_cast<std::uint64_t>(payloads.size()) & 0x7FFFu;
  lo |= static_cast<std::uint64_t>(prim & 0x7FFu) << 47;
  lo |= 1ull << 60;  // NREG = 1
  const std::uint64_t hi = descriptors[0] & 0xFu;
  for (int index = 0; index < 8; ++index) gif.push_back(static_cast<std::uint8_t>((lo >> (8 * index)) & 0xFF));
  for (int index = 0; index < 8; ++index) gif.push_back(static_cast<std::uint8_t>((hi >> (8 * index)) & 0xFF));
  for (const auto &payload : payloads) {
    for (int index = 0; index < 8; ++index)
      gif.push_back(static_cast<std::uint8_t>((payload[0] >> (8 * index)) & 0xFF));
    for (int index = 0; index < 8; ++index)
      gif.push_back(static_cast<std::uint8_t>((payload[1] >> (8 * index)) & 0xFF));
  }
}

// A+D write: the tag descriptor 0xE selects the A+D path and the payload high qword's low 7 bits name the target
// register (coregs3_gif.cpp read_register case 0xE).
std::array<std::uint64_t, 2> ad_write(std::uint32_t address, std::uint64_t data) {
  return {data, static_cast<std::uint64_t>(address & 0x7Fu)};
}

std::uint64_t tex0_word(std::uint32_t tbp0, std::uint32_t cbp, std::uint32_t cld) {
  return static_cast<std::uint64_t>(tbp0) | (2ull << 14) | (0x14ull << 20) | (4ull << 26) | (4ull << 30) |
         (1ull << 34) | (static_cast<std::uint64_t>(cbp) << 37) | (0ull << 51) | (0ull << 55) | (0ull << 56) |
         (static_cast<std::uint64_t>(cld) << 61);
}

// The tap sequence of one reflection job: the STQ writes, then the GS state writes, then four XYZF2 vertices. The state
// is the measured main-view reflection form: TEX0 CLD = 0, TEX1 nearest/nearest MXL 0, CLAMP repeat, TEX2 CLD = 1 T4
// load, TEST 0x5820C (ATE off, DATE off, ZTST GEQUAL), ALPHA 0x44, FRAME FBMSK 0x00FFFFFF, ZBUF ZMSK 1, FOGCOL
// 0x27272A, XYOFFSET the main view.
std::vector<std::uint8_t> make_tap_stream(bool with_tex2) {
  const std::uint64_t tex0 = tex0_word(0x3F28u, 0x3F70u, 0u);
  const std::uint64_t tex2 = tex0_word(0x3F28u, 0x3F70u, 1u);
  std::vector<std::uint8_t> tap;
  // STQ (register 0x2), one per vertex, latched into the XYZF2 records that follow.
  const float uvs[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.75f, 0.75f}, {0.25f, 0.75f}};
  std::vector<std::array<std::uint64_t, 2>> stq;
  for (const auto &uv : uvs) {
    const std::uint64_t lo = static_cast<std::uint64_t>(float_bits(uv[0])) |
                             (static_cast<std::uint64_t>(float_bits(uv[1])) << 32);
    stq.push_back({lo, static_cast<std::uint64_t>(float_bits(1.0f))});
  }
  put_packed_tag(tap, 0x3C, std::vector<std::uint32_t>(stq.size(), 0x2u), stq);
  // The state writes.
  std::vector<std::array<std::uint64_t, 2>> writes;
  writes.push_back(ad_write(0x06u, tex0));
  writes.push_back(ad_write(0x14u, 0ull));  // TEX1: MXL 0, MMAG 0, MMIN 0
  writes.push_back(ad_write(0x08u, 0ull));  // CLAMP: repeat, whole texture
  if (with_tex2) writes.push_back(ad_write(0x16u, tex2));
  writes.push_back(ad_write(0x47u, 0x0005820Cull));
  writes.push_back(ad_write(0x42u, 0x44ull));
  writes.push_back(ad_write(0x4Cu, 0x00FFFFFF000A00D2ull));
  writes.push_back(ad_write(0x4Eu, 0x0000000101000000ull));
  writes.push_back(ad_write(0x3Du, 0x0027272Aull));
  writes.push_back(ad_write(0x18u, 0x0000720800006C08ull));
  put_packed_tag(tap, 0x3C, std::vector<std::uint32_t>(writes.size(), 0xEu), writes);
  // Four vertices (XYZF2), all at the same 12.4 point; the fixture only needs their count and STQ.
  std::vector<std::array<std::uint64_t, 2>> vertices;
  for (int index = 0; index < 4; ++index) {
    const std::uint64_t lo = 0x0100ull | (0x0100ull << 32);
    vertices.push_back({lo, 120ull << 36});  // fog 120, z 0
  }
  put_packed_tag(tap, 0x3C, std::vector<std::uint32_t>(vertices.size(), 0x4u), vertices);
  return tap;
}

SlmImage make_image(std::uint32_t palette_word) {
  SlmImage image;
  image.cover(vram::Window{0u, vram::kBlocksTotal});
  std::fill(image.local.bytes().begin(), image.local.bytes().end(), 0u);
  for (std::uint32_t block = 0x3F70u; block < 0x3F74u; ++block)
    for (std::uint32_t word = 0; word < vram::kBlockBytes / 4u; ++word)
      put_u32_at(image.local.bytes().data() + block * vram::kBlockBytes + word * 4u, palette_word);
  return image;
}

void check_binding(bool with_tex2) {
  FrameSnapshot frame;
  frame.vblank = 7398;
  const std::vector<std::uint8_t> packet = make_world_packet(4);
  frame.packets = packet;
  CapturedDraw draw = make_draw(165, kPassReflection, packet, 0x014DD970u);
  set_program(draw, 0x0080u);
  frame.draws.push_back(draw);

  const std::uint32_t palette_word = 0x40302010u;
  SlmImage default_image = make_image(0x11223344u);
  SlmImage job_image = make_image(palette_word);

  VramRecord job_record;
  job_record.kind = vram::kRecordJobTexture;
  job_record.label = 460387u;
  job_record.windows = {{0x3F28u, 32u}, {0x3F70u, 4u}};
  job_record.bytes.resize((32u + 4u) * vram::kBlockBytes, 0u);
  std::memcpy(job_record.bytes.data(), job_image.local.bytes().data() + 0x3F28u * vram::kBlockBytes,
              32u * vram::kBlockBytes);
  std::memcpy(job_record.bytes.data() + 32u * vram::kBlockBytes,
              job_image.local.bytes().data() + 0x3F70u * vram::kBlockBytes, 4u * vram::kBlockBytes);
  std::map<std::uint32_t, const VramRecord *> job_records;
  job_records[460387u] = &job_record;
  JobTextureImages job_images;
  job_images.records = &job_records;

  const std::vector<std::uint8_t> tap_stream = make_tap_stream(with_tex2);
  TapFileRecord gif_record;
  gif_record.kind = kTapKindGif;
  gif_record.job = 460387u;
  gif_record.vblank = frame.vblank;
  gif_record.bytes = static_cast<std::uint32_t>(tap_stream.size());
  TapFileRecord source_record;
  source_record.kind = kTapKindSource;
  source_record.job = 460387u;
  source_record.vblank = frame.vblank;
  source_record.address = 0x014DD970u;
  std::vector<TapFileRecord> tap_records{source_record, gif_record};

  TextureFrameOptions options;
  options.use_tap = true;
  options.world_channels = true;
  options.job_textures = &job_images;
  FrameTextureTable table;
  TextureFrameCounters counters;
  std::string error;
  require(build_frame_texture_table(frame, default_image, tap_stream.data(), tap_records, options, table, counters,
                                    error),
          "the reflection binding table builds");
  require(table.draws.size() == 1 && table.draws[0].size() == 1, "one reflection item");
  const TextureBinding &binding = table.draws[0][0];
  if (with_tex2) {
    require(binding.texture != nullptr, "the reflection item is textured from its tapped state");
    require(binding.texture->from_state, "the sampler came from the tapped state, not the record");
    require(binding.texture->palette_via_tex2, "the CLD = 0 palette came from the TEX2 load");
    require(counters.world_job_slm == 1, "the item decoded against its own job record");
    require(counters.world_state_palette_tex2 == 1, "the TEX2 palette path is counted");
    require(binding.state.test_known && binding.state.test == 0x0005820Cull, "the tapped TEST");
    require(binding.state.alpha_known && binding.state.alpha == 0x44ull, "the tapped ALPHA");
    require(binding.state.frame_known && binding.state.fbmsk == 0x00FFFFFFu, "the alpha-only FRAME.FBMSK");
    require(binding.state.zbuf_known && binding.state.zmsk == 1u, "the no-depth-write ZBUF.ZMSK");
    require(binding.state.prim_known && binding.state.prim == 0x3Cu, "the tapped PRIM");
    require(binding.state.xyoffset_known && binding.state.xyoffset == kMainViewXyOffset, "the main-view XYOFFSET");
    require(binding.vertex_fog.size() == 4 && binding.vertex_fog[0] == 120, "the per-vertex tapped fog");
    // The decoded texel is the job record's palette word, not the default image's (index 0; R in the low byte).
    require(binding.texture->rgba[0] == (palette_word & 0xFFu) &&
                binding.texture->rgba[1] == ((palette_word >> 8) & 0xFFu) &&
                binding.texture->rgba[2] == ((palette_word >> 16) & 0xFFu) &&
                binding.texture->rgba[3] == ((palette_word >> 24) & 0xFFu),
            "the reflection item sampled its job record's pixels");
  } else {
    // CLD = 0 without the TEX2 write: the state is refused and counted, never silently substituted with the record.
    require(binding.texture == nullptr, "a CLD = 0 state without TEX2 stays untextured");
    require(counters.world_state_refused == 1, "the missing TEX2 refusal is counted");
    require(!counters.state_refusals.empty(), "the refusal is named");
  }
}

// ---- The render integration ----

struct SolidTexture {
  std::vector<std::uint8_t> rgba;
  SampledTexture sampler;
  TextureBinding binding;
};

void fill_solid(SolidTexture &solid, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t alpha) {
  solid.rgba.assign(16u * 16u * 4u, 0u);
  for (std::size_t pixel = 0; pixel < 16u * 16u; ++pixel) {
    solid.rgba[pixel * 4 + 0] = r;
    solid.rgba[pixel * 4 + 1] = g;
    solid.rgba[pixel * 4 + 2] = b;
    solid.rgba[pixel * 4 + 3] = alpha;
  }
  solid.sampler = SampledTexture{};
  solid.sampler.psm = 0x14u;
  solid.sampler.tw = 4;
  solid.sampler.th = 4;
  solid.sampler.tcc = 1;
  solid.sampler.tfx = 0;
  solid.sampler.mxl = 0;
  solid.sampler.mmag = 0;
  solid.sampler.mmin = 0;
  solid.sampler.wms = 0;
  solid.sampler.wmt = 0;
  solid.sampler.width = 16;
  solid.sampler.height = 16;
  solid.sampler.rgba = solid.rgba.data();
  solid.binding = TextureBinding{};
  solid.binding.texture = &solid.sampler;
  solid.binding.state = measured_sample_state();
  solid.binding.state.from_tap = true;
  solid.binding.state.test = 0x0005820Cull;
  solid.binding.state.test_known = true;
  solid.binding.state.alpha = 0x44ull;
  solid.binding.state.alpha_known = true;
  solid.binding.state.zmsk = 0;
  solid.binding.state.zbuf_known = true;
  solid.binding.state.prim = 0x3Cu;  // TME on, ABE off
  solid.binding.state.prim_known = true;
  solid.binding.state.fbmsk = 0;
  solid.binding.state.frame_known = true;
}

struct AbImage {
  std::vector<std::uint8_t> rgba;
  std::vector<std::uint8_t> mask;
  RenderStats stats;
};

// Renders one course quad, optionally with the same quad as a reflection-pass draw. `wet_depth` scales the wet draw's
// clip position and w together, so the screen footprint is unchanged and only the view depth differs.
AbImage render_ab(const std::vector<std::uint8_t> &packet, const SolidTexture &course_solid,
                  const SolidTexture &wet_solid, bool with_wet, float wet_depth) {
  FrameSnapshot frame;
  frame.vblank = 7398;
  frame.packets = packet;
  CapturedDraw course = make_draw(10, kPassCourse, packet, 0x1000u);
  set_program(course, 0x0080u);
  frame.draws.push_back(course);
  if (with_wet) {
    CapturedDraw wet = make_draw(11, kPassReflection, packet, 0x2000u);
    set_program(wet, 0x0080u);
    wet.matrix_packet[1][0] = float_bits(wet_depth / 8192.0f);
    wet.matrix_packet[2][1] = float_bits(wet_depth / 8192.0f);
    wet.matrix_packet[4][0] = float_bits(-0.5f * wet_depth);
    wet.matrix_packet[4][1] = float_bits(-0.5f * wet_depth);
    wet.matrix_packet[4][3] = float_bits(wet_depth);
    frame.draws.push_back(wet);
  }
  FrameTextureTable table;
  table.sequence = {10, 11};
  table.draws.push_back({course_solid.binding});
  if (with_wet) table.draws.push_back({wet_solid.binding});
  else table.draws.push_back({});
  RenderOptions options;
  options.width = 64;
  options.height = 48;
  options.world_pass = true;
  options.textures = &table;
  AbImage image;
  std::string error;
  require(render_frame(frame, options, image.rgba, image.mask, image.stats, error), "the A/B render succeeds");
  return image;
}

void check_render() {
  const std::vector<std::uint8_t> packet = make_world_packet(4);
  SolidTexture course_solid, wet_solid;
  fill_solid(course_solid, 80, 90, 100, 255);
  fill_solid(wet_solid, 30, 40, 50, 100);  // distinct colour and alpha: only the alpha lane may move
  wet_solid.binding.state.fbmsk = 0x00FFFFFFu;
  wet_solid.binding.state.zmsk = 1;

  const AbImage base = render_ab(packet, course_solid, wet_solid, false, 1.0f);
  const AbImage wet = render_ab(packet, course_solid, wet_solid, true, 1.0f);
  require(wet.stats.world_reflection_draws == 1, "the reflection draw is counted");
  require(wet.stats.world_reflection_items == 1, "the reflection item is counted");
  require(wet.stats.world_reflection_vertices == 4, "the reflection vertices are counted");
  require(wet.stats.world_reflection_pixels > 0, "the reflection pass wrote fragments");
  require(wet.stats.world_reflection_alpha_only == wet.stats.world_reflection_pixels,
          "every reflection fragment wrote destination alpha only (FBMSK 0x00FFFFFF)");
  require(wet.stats.world_reflection_rgb_only == 0, "no reflection fragment wrote RGB");
  require(wet.stats.depth_masked_pixels >= wet.stats.world_reflection_pixels, "the pass wrote no depth (ZBUF.ZMSK)");
  std::size_t rgb_differ = 0, alpha_differ = 0, wet_masked = 0;
  for (std::size_t pixel = 0; pixel < base.rgba.size() / 4; ++pixel) {
    if (base.rgba[pixel * 4 + 0] != wet.rgba[pixel * 4 + 0] || base.rgba[pixel * 4 + 1] != wet.rgba[pixel * 4 + 1] ||
        base.rgba[pixel * 4 + 2] != wet.rgba[pixel * 4 + 2])
      ++rgb_differ;
    if (base.rgba[pixel * 4 + 3] != wet.rgba[pixel * 4 + 3]) ++alpha_differ;
    if (wet.mask[pixel] != 0) ++wet_masked;
  }
  require(rgb_differ == 0, "the destination-alpha pass left every RGB byte unchanged");
  require(wet_masked > 0, "the reflection pass covered pixels");
  require(alpha_differ == wet_masked,
          "exactly the reflection pass's covered pixels changed the alpha lane");

  // A reflection quad at a larger view depth than the course fails the measured GEQUAL test and is counted.
  const AbImage behind = render_ab(packet, course_solid, wet_solid, true, 2.0f);
  require(behind.stats.world_reflection_pixels == 0, "the farther reflection quad wrote nothing");
  require(behind.stats.world_reflection_depth_failed > 0, "its fragments are counted as depth-rejected");
}

}  // namespace

int main() {
  try {
    check_selection();
    check_binding(true);
    check_binding(false);
    check_render();
    std::printf("coregs3 wet reflection fixture: OK\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("coregs3 wet reflection fixture: %s\n", error.what());
    return 1;
  }
}
