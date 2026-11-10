// CTest native_coregs3: the TASK-090 phase 1 offline path, host only. No game data is read: every packet here is
// hand-authored, which is what makes these fixtures publishable.
//   - the `.gt4scene` frame block round trip, and the refusals (magic, an older version, truncated block, a packet range
//     outside the block, an item count above the cap, a light block above its cap);
//   - the PGL output chain walker (format version 4): per-item light uploads, the carried block of a draw that uploads
//     nothing, the uncertain carried block after a cursor discontinuity, the per-item run pairs against the terminating
//     pair, and the refusals (a mismatched item tag, an unknown DMA tag id);
//   - the bounded VIF decoder: an ordinary V3-16 UNPACK into the TOPS window, an absolute V4-8 UNPACK, STROW with a fill
//     cycle, and the refusals (unknown opcode, payload overrun);
//   - the channel rule: destination/format classification, the V3-8 normal channel at TOPS 0x40 (signed bytes and the raw
//     V3-16 position) and the strip topology of a mesh;
//   - the measured draw transform (the 0xC1 columns c0..c3 = vf24..vf27 with the linear part scaled by the shape's
//     `value` and by the position encoding's units; docs/COREGS3-HOOKS.md §10.2);
//   - the measured VU1 lighting in the offline renderer (coregs3_lighting.hpp): a body/paint draw is lit and a draw
//     without the block, the normals or the colour channel keeps the base colour and is counted as a fallback;
//   - the live present ring: the newest published frame is claimed once, with the O(1) buffer swap
//     (docs/COREGS3-HOOKS.md §10.3);
//   - a PNG file: signature, IHDR size, IEND.
#include "coregs3_chain.hpp"
#include "coregs3_png.hpp"
#include "coregs3_present.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vif.hpp"

#include <array>
#include <cmath>
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

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

// A VIF UNPACK command word: opcode 0x60 | vn<<2 | vl, plus the TOPS bit, NUM and the destination in qwords.
std::uint32_t unpack_destination(std::uint32_t qword) { return qword & 0x3FFu; }

std::uint32_t unpack_word(std::uint32_t vn, std::uint32_t vl, std::uint32_t num, std::uint32_t qword, bool tops) {
  return 0x60000000u | ((vn & 3u) << 26) | ((vl & 3u) << 24) | ((num & 0xFFu) << 16) | (tops ? 0x8000u : 0u) |
         unpack_destination(qword);
}

// Bit 28 enables STMASK for this UNPACK (the donor's `masked` bit; the GT4 strip-table write uses it).
std::uint32_t mask_unpack(std::uint32_t word) { return word | 0x10000000u; }

// A packet with `vertices` V3-16 positions at TOPS qword 0 and, when `table` is not empty, an STMASK 0x3F3F3F3F plus a
// masked V1-8 strip table at TOPS qword 0x40 (lane 3 only, exactly the shape the capture showed).
std::vector<std::uint8_t> make_strip_packet(std::uint32_t vertices, const std::vector<std::uint8_t> &table) {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, static_cast<std::uint32_t>(vertices), 0x00, true));
  for (std::uint32_t index = 0; index < vertices; ++index) {
    put_u16(packet, static_cast<std::uint16_t>(index * 1024));
    put_u16(packet, 0);
    put_u16(packet, 0);
  }
  pad();
  if (!table.empty()) {
    put_u32(packet, 0x20000000u);  // STMASK
    put_u32(packet, 0x3F3F3F3Fu);
    put_u32(packet, mask_unpack(unpack_word(0, 2, static_cast<std::uint32_t>(table.size()), 0x40, true)));
    for (std::uint8_t value : table) packet.push_back(value);
    pad();
  }
  return packet;
}

std::vector<std::uint8_t> make_packet() {
  // 3 positions (V3-16, TOPS qword 0), 3 colours (V4-8, TOPS qword 0x80), and a one-strip strip table [1, 3].
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 3, 0x00, true));
  const std::int16_t positions[3][3] = {{-4096, 0, 0}, {4096, 0, 0}, {0, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(3, 2, 3, 0x80, true));
  for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
    for (std::uint32_t lane = 0; lane < 4; ++lane) packet.push_back(static_cast<std::uint8_t>(0x40 + vertex * 8));
  pad();
  put_u32(packet, 0x20000000u);  // STMASK
  put_u32(packet, 0x3F3F3F3Fu);
  put_u32(packet, mask_unpack(unpack_word(0, 2, 2, 0x40, true)));
  packet.push_back(1);  // one strip
  packet.push_back(3);  // three times one triangle -> three vertices
  pad();
  return packet;
}

// A packet for the body/paint family channel set: V3-16 positions at TOPS qword 0 and a V3-8 normal channel at TOPS
// qword 0x40. The normals are chosen to exercise sign extension (0x7F = 127, 0x80 = -128, 0xFF = -1).
std::vector<std::uint8_t> make_normal_packet() {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 3, 0x00, true));  // V3-16 positions, TOPS qword 0
  const std::int16_t positions[3][3] = {{-4096, 0, 0}, {4096, 0, 0}, {0, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(2, 2, 3, 0x40, true));  // V3-8 normals, TOPS qword 0x40
  const std::uint8_t normals[3][3] = {{127, 0, 0}, {0, 0x80, 0xFF}, {0, 0, 64}};
  for (const auto &normal : normals)
    for (std::uint8_t byte : normal) packet.push_back(byte);
  while (packet.size() % 16 != 0) packet.push_back(0);  // the DMA REF QWC is whole qwords
  return packet;
}

// The body/paint channel set with a colour channel: V3-16 positions at TOPS qword 0, V3-8 normals at TOPS qword 0x40,
// V4-8 colours at TOPS qword 0x80 and the masked strip table at TOPS qword 0x40 (lane 3 only). The three positions put
// the vertices at (-0.5, 0), (0.5, 0) and (0, 0.5) in model metres, inside a 64x48 viewport for the point check below.
std::vector<std::uint8_t> make_lit_packet() {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 3, 0x00, true));  // V3-16 positions, TOPS qword 0
  const std::int16_t positions[3][3] = {{-2048, 0, 0}, {2048, 0, 0}, {0, 2048, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(2, 2, 3, 0x40, true));  // V3-8 normals, TOPS qword 0x40
  const std::uint8_t normals[3][3] = {{0, 0, 127}, {0, 0, 127}, {0, 0, 127}};
  for (const auto &normal : normals)
    for (std::uint8_t byte : normal) packet.push_back(byte);
  pad();
  put_u32(packet, unpack_word(3, 2, 3, 0x80, true));  // V4-8 colours, TOPS qword 0x80
  const std::uint8_t colours[3][4] = {{64, 128, 32, 255}, {64, 128, 32, 255}, {64, 128, 32, 255}};
  for (const auto &colour : colours)
    for (std::uint8_t byte : colour) packet.push_back(byte);
  pad();
  put_u32(packet, 0x20000000u);  // STMASK
  put_u32(packet, 0x3F3F3F3Fu);
  put_u32(packet, mask_unpack(unpack_word(0, 2, 2, 0x40, true)));
  packet.push_back(1);  // one strip
  packet.push_back(3);  // three times one triangle -> three vertices
  pad();
  return packet;
}

void set_identity_columns(CapturedDraw &draw) {
  // The measured metre transform is the identity: vf24..vf27 are the identity and the shape `value` is 1/4096, so the
  // V3-16 metre scale (value * 4096) is exactly 1 and clip = (x, y, z, 1).
  draw.cmd_value = float_bits(1.0f / 4096.0f);
  for (std::uint32_t column = 0; column < 4; ++column)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.vf[column][lane] = float_bits(column == lane ? 1.0f : 0.0f);
}

// The measured six-qword body/paint block, as the capture carries it: LP far away on +z, C3 = (1,1,1,1), C4 = (0.8)^3.
void set_body_paint_block(CapturedDraw &draw) {
  const float block[6][4] = {
      {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 100000.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.8f, 0.8f, 0.8f, 0.0f},
  };
  draw.light_block_qwords = 6;
  draw.light_block_command = 0x6C06C0C9u;
  draw.light_block_source = 0x00130000u;
  for (std::uint32_t qword = 0; qword < 6; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane) draw.light_block[qword][lane] = float_bits(block[qword][lane]);
  // The renderers read the per-item state: the measured body/paint selector and the item's own block.
  draw.program_itop = 0x0400020Bu;
  draw.program_dispatch = 0x1058u;
  if (draw.item_count != 0) {
    CapturedItem &item = draw.items[0];
    item.light_block_qwords = 6;
    item.light_block_command = 0x6C06C0C9u;
    item.light_block_source = 0x00130000u;
    item.light_source = kLightItemUploaded;
    for (std::uint32_t qword = 0; qword < 6; ++qword)
      for (std::uint32_t lane = 0; lane < 4; ++lane) item.light_block[qword][lane] = float_bits(block[qword][lane]);
    item.run_itop = 0x0400020Bu;
    item.run_mscal = 0x140003A8u;
    item.run_valid = 1;
  }
}

CapturedDraw make_draw(std::uint32_t item_packet_bytes) {
  CapturedDraw draw;
  draw.sequence = 7;
  draw.shape = 0x00400000u;  // a guest address, never dereferenced by this fixture
  draw.cmd_value = float_bits(1.0f);
  draw.light_block_qwords = 6;
  draw.light_block_command = 0x6C06C0C9u;  // V4-32, six qwords at TOPS+0xC9 (the body/paint form)
  draw.light_block_source = 0x00130000u;
  for (std::uint32_t qword = 0; qword < draw.light_block_qwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.light_block[qword][lane] = float_bits(static_cast<float>(qword * 4 + lane));
  draw.program_tag = 0x140003A8u;     // VIF MSCAL, immediate 0x3A8
  draw.program_selector = 0x3A8u;
  draw.program_entry = 0x1D40u;       // selector * 8: the MSCAL entry stub in the VU1 image
  draw.program_itop = 0x0400020Bu;    // VIF ITOP, immediate 0x20B: dispatch 0x1058, the body/paint program
  draw.program_dispatch = 0x1058u;    // ITOP * 8: the entry the stub dispatches to
  draw.program_count = 881u;          // PGL obj->count (+0x14)
  draw.program_valid = 1;
  draw.chain_tags = 9;
  draw.chain_valid = 1;
  draw.program_uniform = 1;
  draw.pass = kPassCars;
  draw.call_site = kCallSiteModelSet2;
  draw.caller_ra = 0x004556A8u;
  draw.vf[0][0] = float_bits(2.0f);
  draw.vf[0][1] = float_bits(0.0f);
  draw.vf[0][2] = float_bits(0.0f);
  draw.vf[0][3] = float_bits(3.0f);
  draw.vf[1][1] = float_bits(4.0f);
  draw.vf[2][2] = float_bits(5.0f);
  draw.vf[2][3] = float_bits(-1.0f);
  draw.vf[3][3] = float_bits(1.0f);
  draw.item_count = 1;
  draw.items[0].address = 0x00100000u;
  draw.items[0].qwords = static_cast<std::uint16_t>(item_packet_bytes / 16);
  draw.items[0].event = 1u | (1u << 9);
  draw.items[0].texture_record = 0;
  draw.items[0].material_record = 0;
  draw.items[0].packet_offset = 0;
  draw.items[0].packet_bytes = item_packet_bytes;
  // Format version 4: the per-item light state and run pair.
  draw.items[0].light_block_qwords = draw.light_block_qwords;
  draw.items[0].light_block_command = draw.light_block_command;
  draw.items[0].light_block_source = draw.light_block_source;
  draw.items[0].light_source = kLightItemUploaded;
  for (std::uint32_t qword = 0; qword < draw.light_block_qwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.items[0].light_block[qword][lane] = draw.light_block[qword][lane];
  draw.items[0].run_itop = draw.program_itop;
  draw.items[0].run_mscal = draw.program_tag;
  draw.items[0].run_valid = 1;
  return draw;
}

void check_capture_round_trip() {
  FrameSnapshot frame;
  frame.index = 3;
  frame.vblank = 2450;
  frame.camera.valid = 7;
  frame.camera.camera = 0x00401234u;
  frame.camera.pgl_block_k3[0] = 0x3F800000u;
  // Format version 5: the scene environment block and the per-pass GS state journal.
  frame.environment.valid = 1;
  frame.environment.words[0] = 0x41200000u;                 // a sun value
  frame.environment.words[kEnvWords - 1] = 0x3F000000u;     // the last light word
  CapturedPassState pass_state;
  pass_state.pass = kPassCars;
  pass_state.first_draw = 7;
  pass_state.draw_count = 1;
  pass_state.item_count = 1;
  pass_state.vblank = 2450;
  pass_state.fogcol = 0x000000000027272Aull;
  pass_state.frame = 0xFF000000000A00D2ull;
  pass_state.xyoffset = 0x00007C0800007C08ull;
  pass_state.zbuf = 0x0000000101000000ull;
  pass_state.scissor = 0x01BF0000027F0000ull;
  pass_state.alpha = 0x0000000000000044ull;
  pass_state.test = 0x000000000005820Cull;
  frame.pass_states.push_back(pass_state);
  CapturedTexture texture;
  texture.address = 0x00110000u;
  texture.bytes[0] = 0xAB;
  frame.textures.push_back(texture);
  CapturedMaterial material;
  material.address = 0x00120000u;
  material.bytes[7] = 0xCD;
  frame.materials.push_back(material);
  frame.packets = make_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(frame.packets.size()));
  frame.draws.push_back(draw);
  frame.draw_count = 1;
  frame.texture_count = 1;
  frame.material_count = 1;

  std::vector<std::uint8_t> block;
  require(frame_block_bytes(frame, block), "the frame block must serialize");
  FrameSnapshot read;
  std::string error;
  const std::size_t consumed = parse_frame_block(block.data(), block.size(), kSceneVersion, read, error);
  require(consumed == block.size(), "the block must consume exactly its own bytes");
  require(read.index == frame.index && read.vblank == frame.vblank, "index and vblank survive");
  require(read.draw_count == 1 && read.draws.size() == 1, "one draw survives");
  require(read.draws[0].shape == draw.shape && read.draws[0].cmd_value == draw.cmd_value, "the Cmd header survives");
  require(read.draws[0].items[0].event == draw.items[0].event, "the item event survives");
  require(read.draws[0].items[0].packet_bytes == draw.items[0].packet_bytes, "the packet length survives");
  require(read.draws[0].light_block_qwords == draw.light_block_qwords &&
              read.draws[0].light_block_command == draw.light_block_command &&
              read.draws[0].light_block_source == draw.light_block_source,
          "the light block header survives");
  require(read.draws[0].light_block[5][3] == draw.light_block[5][3], "the light block qwords survive");
  require(read.draws[0].light_block[6][0] == 0, "the unused light block qwords stay zero");
  require(read.draws[0].program_tag == draw.program_tag &&
              read.draws[0].program_selector == draw.program_selector &&
              read.draws[0].program_entry == draw.program_entry &&
              read.draws[0].program_itop == draw.program_itop &&
              read.draws[0].program_dispatch == draw.program_dispatch &&
              read.draws[0].program_count == draw.program_count &&
              read.draws[0].program_valid == draw.program_valid,
          "the VU1 program selector survives");
  require(read.draws[0].chain_tags == draw.chain_tags && read.draws[0].chain_valid == draw.chain_valid &&
              read.draws[0].program_uniform == draw.program_uniform,
          "the per-draw chain summary survives");
  require(read.draws[0].items[0].light_block_qwords == draw.items[0].light_block_qwords &&
              read.draws[0].items[0].light_block_command == draw.items[0].light_block_command &&
              read.draws[0].items[0].light_block_source == draw.items[0].light_block_source &&
              read.draws[0].items[0].light_source == draw.items[0].light_source,
          "the per-item light header survives");
  require(read.draws[0].items[0].light_block[5][3] == draw.items[0].light_block[5][3],
          "the per-item light qwords survive");
  require(read.draws[0].items[0].run_itop == draw.items[0].run_itop &&
              read.draws[0].items[0].run_mscal == draw.items[0].run_mscal &&
              read.draws[0].items[0].run_valid == draw.items[0].run_valid,
          "the per-item run pair survives");
  require(read.packets == frame.packets, "the packet bytes survive");
  require(read.textures[0].address == texture.address && read.textures[0].bytes[0] == 0xAB, "the texture survives");
  require(read.materials[0].address == material.address && read.materials[0].bytes[7] == 0xCD, "the material survives");
  require(read.camera.valid == 7 && read.camera.camera == 0x00401234u, "the camera record survives");
  require(read.draws[0].pass == kPassCars && read.draws[0].call_site == kCallSiteModelSet2 &&
              read.draws[0].caller_ra == 0x004556A8u,
          "the pass, call site and caller return address survive");
  require(read.environment.valid == 1 && read.environment.words[0] == 0x41200000u &&
              read.environment.words[kEnvWords - 1] == 0x3F000000u,
          "the scene environment block survives");
  require(read.pass_states.size() == 1 && read.pass_states[0].pass == kPassCars &&
              read.pass_states[0].first_draw == 7 && read.pass_states[0].draw_count == 1 &&
              read.pass_states[0].item_count == 1 && read.pass_states[0].vblank == 2450,
          "the pass journal header survives");
  require(read.pass_states[0].fogcol == pass_state.fogcol && read.pass_states[0].frame == pass_state.frame &&
              read.pass_states[0].xyoffset == pass_state.xyoffset && read.pass_states[0].zbuf == pass_state.zbuf &&
              read.pass_states[0].scissor == pass_state.scissor && read.pass_states[0].alpha == pass_state.alpha &&
              read.pass_states[0].test == pass_state.test,
          "the per-pass GS register shadow survives");

  // A block whose item points outside the packet block must be refused, not read.
  FrameSnapshot broken = frame;
  broken.draws[0].items[0].packet_offset = static_cast<std::uint32_t>(broken.packets.size()) - 4;
  std::vector<std::uint8_t> broken_block;
  require(!frame_block_bytes(broken, broken_block), "an item range outside the packet block is refused");

  // A light block above the format's cap must be refused by the writer, never truncated.
  FrameSnapshot over_cap = frame;
  over_cap.draws[0] = draw;
  over_cap.draws[0].light_block_qwords = kMaxLightBlockQwords + 1u;
  require(!frame_block_bytes(over_cap, broken_block), "a light block above the cap is refused");
  // The same bound applies to the per-item block (format version 4).
  FrameSnapshot over_item_cap = frame;
  over_item_cap.draws[0] = draw;
  over_item_cap.draws[0].items[0].light_block_qwords = kMaxLightBlockQwords + 1u;
  require(!frame_block_bytes(over_item_cap, broken_block), "a per-item light block above the cap is refused");
  // Format version 5 bounds: a pass id outside the enum, a journal above its cap and an inconsistent journal entry
  // are refused by the writer, never written.
  FrameSnapshot bad_pass = frame;
  bad_pass.draws[0].pass = kPassCount;
  require(!frame_block_bytes(bad_pass, broken_block), "a pass id outside the enum is refused");
  FrameSnapshot bad_site = frame;
  bad_site.draws[0].call_site = kCallSiteTachometer + 1u;
  require(!frame_block_bytes(bad_site, broken_block), "a call site outside the enum is refused");
  FrameSnapshot bad_state_pass = frame;
  bad_state_pass.pass_states[0].pass = kPassCount;
  require(!frame_block_bytes(bad_state_pass, broken_block), "a journal pass id outside the enum is refused");
  FrameSnapshot bad_state_entry = frame;
  bad_state_entry.pass_states[0].first_draw = kNoRecord;
  bad_state_entry.pass_states[0].draw_count = 1;
  require(!frame_block_bytes(bad_state_entry, broken_block), "an empty journal entry with draws is refused");
  FrameSnapshot over_states = frame;
  over_states.pass_states.resize(kMaxPassStatesPerFrame + 1u, pass_state);
  require(!frame_block_bytes(over_states, broken_block), "a pass journal above its cap is refused");
  FrameSnapshot bad_environment = frame;
  bad_environment.environment.valid = 2;
  require(!frame_block_bytes(bad_environment, broken_block), "an environment validity flag above 1 is refused");
  // The reader refuses a corrupted journal count (the pass count sits after the 36-byte frame header, the 264-byte
  // camera record and the 404-byte environment record: block offset 8 + 704).
  std::vector<std::uint8_t> over_journal = block;
  const std::size_t journal_count_at = 8u + 36u + 264u + 404u;
  require(journal_count_at + 4 <= over_journal.size(), "the fixture knows the journal count offset");
  over_journal[journal_count_at + 0] = static_cast<std::uint8_t>(kMaxPassStatesPerFrame + 1u);
  over_journal[journal_count_at + 1] = 0;
  over_journal[journal_count_at + 2] = 0;
  over_journal[journal_count_at + 3] = 0;
  require(parse_frame_block(over_journal.data(), over_journal.size(), kSceneVersion, read, error) == 0,
          "a journal count above the cap is refused by the reader");

  SceneHeader header;
  std::vector<std::uint8_t> file;
  write_scene_header(file, SceneHeader{});
  file.insert(file.end(), block.begin(), block.end());
  require(parse_scene_header(file.data(), file.size(), header, error), "the header parses");
  require(header.target == kCaptureTargetV200, "the target is v2.00");
  std::vector<std::uint8_t> bad_magic = file;
  bad_magic[0] = 0;
  SceneHeader ignored;
  require(!parse_scene_header(bad_magic.data(), bad_magic.size(), ignored, error), "a bad magic is refused");
  std::vector<std::uint8_t> bad_version = file;
  bad_version[4] = 9;
  require(!parse_scene_header(bad_version.data(), bad_version.size(), ignored, error), "an unknown version is refused");
  // The immediately older version (version 3) has no per-item light state and must be refused, not misread.
  std::vector<std::uint8_t> older_version = file;
  older_version[4] = static_cast<std::uint8_t>(kSceneVersion - 1u);
  require(!parse_scene_header(older_version.data(), older_version.size(), ignored, error),
          "a capture from the previous format version is refused");
  const std::size_t truncated = block.size() / 2;
  require(parse_frame_block(block.data(), truncated, kSceneVersion, read, error) == 0,
          "a truncated block is refused");
}

void check_vif_decoder() {
  const std::vector<std::uint8_t> bytes = make_packet();
  const VifPacket packet = decode_packet(bytes.data(), bytes.size());
  require(packet.ok, "the hand-authored packet decodes");
  require(packet.commands.size() == 3, "three UNPACK commands");
  require(packet.commands[0].tops && packet.commands[0].destination == 0, "positions go to the TOPS window at 0");
  require(packet.commands[0].format == ChannelFormat::v3_16, "positions are V3-16");
  require(packet.commands[1].destination == 0x80 && packet.commands[1].format == ChannelFormat::v4_8,
          "colours are V4-8 at TOPS + 0x80");
  require(packet.commands[2].tops && packet.commands[2].destination == 0x40, "the strip table is at TOPS + 0x40");
  require(packet.commands[2].format == ChannelFormat::v1_8, "the strip table is V1-8");
  require(packet.commands[2].masked && packet.commands[2].stmask == 0x3F3F3F3Fu,
          "the strip table write enables STMASK 0x3F3F3F3F");

  std::vector<VifVector> positions;
  require(read_vu_vectors(packet.packed.data(), 0, 3, positions), "the positions are readable");
  // TASK-090 track M1: the measured UNPACK sign rule. This packet's V3-16 command carries bit 14 = 0, so the 16-bit
  // element is sign-extended into the 32-bit lane (the course delta chain needs it before the MODE 2 accumulation);
  // bit 14 = 1 keeps the zero-widened value.
  require(packet.commands[0].usn == 0, "the position UNPACK carries the measured sign-extension bit 14 = 0");
  require(positions[0].v[0] == 0xFFFFF000u, "the first x lane is the sign-extended halfword -4096");
  require(positions[1].v[0] == 0x00001000u, "the second x lane keeps 4096");
  require(positions[2].v[1] == 0x00001000u, "the third y lane keeps 4096");
  // V3-16 writes three lanes; the fourth keeps what was there (zero for a fresh image).
  require(positions[0].v[3] == 0, "the unused lane stays zero");

  std::vector<VifVector> colors;
  require(read_vu_vectors(packet.packed.data(), 0x80 * 16, 3, colors), "the colours are readable");
  require(colors[0].v[0] == 0x40u && colors[2].v[3] == 0x50u, "the colour lanes are the packet bytes");

  // STMASK 0x3F3F3F3F writes lane 3 only; the one-component UNPACK is broadcast first, so lane 3 carries the byte and
  // lane 0 keeps whatever was there (zero: nothing else writes qword 0x40 in this packet).
  std::vector<VifVector> table;
  require(read_vu_vectors(packet.packed.data(), 0x40 * 16, 2, table), "the strip table is readable");
  require(table[0].v[3] == 1 && table[1].v[3] == 3, "the strip table bytes land in lane 3");
  require(table[0].v[0] == 0 && table[1].v[0] == 0, "the masked lanes stay untouched");

  // An unknown opcode must be refused instead of skipped.
  std::vector<std::uint8_t> unknown;
  put_u32(unknown, 0x11000000u);
  require(!decode_packet(unknown.data(), unknown.size()).ok, "an unknown opcode is refused");
  // A payload that claims more vectors than the packet holds must be refused.
  std::vector<std::uint8_t> overrun;
  put_u32(overrun, unpack_word(3, 0, 20, 0, false));
  require(!decode_packet(overrun.data(), overrun.size()).ok, "a payload overrun is refused");
  // A fill cycle (CL=1, WL=2): two destination vectors, one source vector, and the filled row takes STROW.
  std::vector<std::uint8_t> fill;
  put_u32(fill, 0x01000201u);  // STCYCL: CL=1 (low byte), WL=2 (next byte)
  put_u32(fill, 0x30000000u);  // STROW
  put_u32(fill, 0xAAAAAAAAu);  // row[0]
  put_u32(fill, 0xBBBBBBBBu);
  put_u32(fill, 0xCCCCCCCCu);
  put_u32(fill, 0xDDDDDDDDu);
  put_u32(fill, unpack_word(0, 0, 2, 0x00, false));  // V1-32, two destination vectors
  put_u32(fill, 0x12345678u);                        // the one source element
  const VifPacket fill_packet = decode_packet(fill.data(), fill.size());
  require(fill_packet.ok, "the fill form decodes");
  std::vector<VifVector> filled;
  require(read_vu_vectors(fill_packet.window.data(), 0, 2, filled), "the filled rows are readable");
  require(filled[0].v[0] == 0x12345678u, "the first vector is the source element");
  require(filled[0].v[1] == 0x12345678u && filled[0].v[3] == 0x12345678u,
          "a one-component UNPACK is broadcast into all four lanes");
  require(filled[1].v[0] == 0xAAAAAAAAu, "the filled vector takes the row register");
}

void check_matrix() {
  CapturedDraw draw = make_draw(0);
  draw.cmd_value = float_bits(0.5f);
  const DrawMatrix matrix = draw_matrix(draw);
  require(matrix.valid, "the matrix is valid");
  // The measured upload (docs/COREGS3-HOOKS.md §10.2): the 0xC1 block is four columns c0..c3 = vf24..vf27, and the
  // guest scales the three linear columns (c0..c2, every lane) by the Cmd float `value`; the translation c3 is not
  // scaled. The make_draw columns are c0 = (2,0,0,3), c1 = (0,4,0,0), c2 = (0,0,5,-1), c3 = (0,0,0,1).
  require(matrix.upload.column[0][0] == 1.0f && matrix.upload.column[0][3] == 1.5f, "c0 is vf24 * value");
  require(matrix.upload.column[1][1] == 2.0f && matrix.upload.column[1][3] == 0.0f, "c1 is vf25 * value");
  require(matrix.upload.column[2][2] == 2.5f && matrix.upload.column[2][3] == -0.5f, "c2 is vf26 * value");
  require(matrix.upload.column[3][3] == 1.0f, "c3 is vf27, never scaled");
  // The row-major form of the upload stores component r of every column in row r. The timeline uses it as the object's
  // 0xC1 matrix (coregs3_timeline.hpp).
  require(matrix.row[0][0] == 1.0f && matrix.row[0][3] == 0.0f, "row 0 is component 0 of every column");
  require(matrix.row[1][1] == 2.0f, "row 1 is component 1");
  require(matrix.row[2][2] == 2.5f && matrix.row[2][3] == 0.0f, "row 2 is component 2");
  require(matrix.row[3][0] == 1.5f && matrix.row[3][2] == -0.5f && matrix.row[3][3] == 1.0f,
          "row 3 is component 3");
  // A V3-16 channel's metre transform multiplies the linear columns by 4096 raw units per metre; a V3-32 channel's
  // stored float needs no such factor.
  require(matrix.integer_positions.column[0][0] == 4096.0f && matrix.integer_positions.column[0][3] == 6144.0f,
          "the V3-16 metre transform carries 4096 * value on c0");
  require(matrix.integer_positions.column[2][2] == 10240.0f, "10240 = 5 * 0.5 * 4096");
  require(matrix.integer_positions.column[3][3] == 1.0f, "the V3-16 translation stays vf27");
  require(matrix.float_positions.column[0][0] == 1.0f && matrix.float_positions.column[0][3] == 1.5f,
          "the V3-32 metre transform carries value only");
  require(matrix.float_positions.column[2][2] == 2.5f, "the V3-32 metre transform of c2 is vf26 * value");

  // A shape `value` of zero is the guest upload's unscaled branch (func_0049E548): the rows are the raw vf block. The
  // metre transform still converts the V3-16 units, so it is not the old unscaled reading either.
  CapturedDraw zero = make_draw(0);
  zero.cmd_value = 0;
  const DrawMatrix unscaled = draw_matrix(zero);
  require(unscaled.upload.column[0][0] == 2.0f && unscaled.upload.column[1][1] == 4.0f &&
              unscaled.upload.column[2][2] == 5.0f && unscaled.upload.column[3][3] == 1.0f,
          "value zero uploads vf24..vf27 unscaled");
  require(unscaled.integer_positions.column[0][0] == 8192.0f, "the V3-16 metre transform still applies 4096");
  require(unscaled.float_positions.column[0][0] == 2.0f, "the V3-32 metre transform is the upload");
  require(unscaled.row[0][0] == 2.0f && unscaled.row[3][0] == 3.0f, "the timeline matrix is the unscaled upload");

  // The control that fails under the old reading: projecting a metre position through the raw vf columns is not the
  // measured transform. With value 0.5 the linear part differs by 2048 on a V3-16 channel.
  const VertexColumns raw = vertex_columns_from_vu0_raw(draw.vf);
  const ProjectionResult corrected = project_vertex(matrix.integer_positions, {1.0f, 2.0f, 0.5f}, 64.0f, 48.0f);
  const ProjectionResult old = project_vertex(raw, {1.0f, 2.0f, 0.5f}, 64.0f, 48.0f);
  require(corrected.status == ProjectionStatus::ok && old.status == ProjectionStatus::ok,
          "both readings project this hand vertex");
  require(std::fabs(corrected.screen.x - old.screen.x) > 1.0f || std::fabs(corrected.screen.y - old.screen.y) > 1.0f,
          "the corrected transform is not the unscaled vf reading");
}

void check_mesh() {
  std::vector<std::uint8_t> packets = make_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packets.size()));
  MeshOptions options;
  Mesh mesh;
  std::string error;
  require(build_mesh(packets.data(), packets.size(), draw, options, mesh, error), "the mesh builds");
  require(mesh.vertices.size() == 3, "three vertices");
  require(mesh.has_colors && mesh.has_uv == false, "colours present, no UV channel");
  require(mesh.has_strip_table, "the strip table is decoded");
  require(mesh.strip_lengths.size() == 1 && mesh.strip_lengths[0] == 3, "one strip of three vertices");
  require(mesh.triangles() == 1, "one triangle");
  require(mesh.vertices[0].position[0] == -1.0f, "the position scale is applied");
  require(mesh.vertices[2].position[1] == 1.0f, "the second lane is Y");

  const ChannelScan scan = scan_channels(packets.data(), packets.size(), draw);
  require(scan.packets == 1 && scan.decoded == 1, "one packet decoded");
  require(scan.channels.size() == 3, "three channels reported");
  require(scan.channels[0].role == ChannelRole::positions, "channel 0 is positions");
  require(scan.channels[1].role == ChannelRole::color, "channel 1 is colour");
  require(scan.channels[2].role == ChannelRole::strip_table, "channel 2 is the strip table");

  // Two strips of three and four vertices: the table says one triangle and two triangles.
  {
    std::vector<std::uint8_t> two_strips = make_strip_packet(7, {2, 3, 6});
    CapturedDraw split = make_draw(static_cast<std::uint32_t>(two_strips.size()));
    Mesh split_mesh;
    require(build_mesh(two_strips.data(), two_strips.size(), split, options, split_mesh, error),
            "a two-strip table builds");
    require(split_mesh.strip_lengths.size() == 2 && split_mesh.strip_lengths[0] == 3 && split_mesh.strip_lengths[1] == 4,
            "the two strip lengths are decoded");
    require(split_mesh.triangles() == 3, "one triangle plus two triangles");
  }

  // The all-zero variant: a table whose first byte is zero means no breaks, one strip over the whole item.
  {
    std::vector<std::uint8_t> zero_table = make_strip_packet(3, {0});
    CapturedDraw whole = make_draw(static_cast<std::uint32_t>(zero_table.size()));
    Mesh whole_mesh;
    require(build_mesh(zero_table.data(), zero_table.size(), whole, options, whole_mesh, error),
            "a zero table builds");
    require(whole_mesh.has_strip_table && whole_mesh.strip_lengths.size() == 1 && whole_mesh.strip_lengths[0] == 3,
            "a zero table means one strip over the item");
  }

  // A length byte that is not three times a triangle count, or a total that does not cover the item, is refused.
  {
    std::vector<std::uint8_t> bad_length = make_strip_packet(3, {1, 4});
    CapturedDraw bad = make_draw(static_cast<std::uint32_t>(bad_length.size()));
    Mesh refused;
    require(!build_mesh(bad_length.data(), bad_length.size(), bad, options, refused, error),
            "a length byte that is not a multiple of three is refused");
    std::vector<std::uint8_t> bad_total = make_strip_packet(3, {1, 9});
    CapturedDraw short_item = make_draw(static_cast<std::uint32_t>(bad_total.size()));
    require(!build_mesh(bad_total.data(), bad_total.size(), short_item, options, refused, error),
            "a table that covers more vertices than the item has is refused");
  }

  // No position channel: refused with a message rather than an empty mesh.
  std::vector<std::uint8_t> colours_only;
  put_u32(colours_only, unpack_word(3, 2, 3, 0x80, true));
  for (int index = 0; index < 12; ++index) colours_only.push_back(0x10);
  CapturedDraw no_positions = make_draw(static_cast<std::uint32_t>(colours_only.size()));
  Mesh empty;
  require(!build_mesh(colours_only.data(), colours_only.size(), no_positions, options, empty, error),
          "a packet without positions is refused");
}

// The V3-8 normal channel at TOPS 0x40 and the raw V3-16 position, hand-authored: the signed bytes must sign-extend and
// the raw position must be the exact halfword the VU1 converts, while position keeps the position_scale conversion.
void check_mesh_normals() {
  std::vector<std::uint8_t> packets = make_normal_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packets.size()));
  MeshOptions options;
  Mesh mesh;
  std::string error;
  require(build_mesh(packets.data(), packets.size(), draw, options, mesh, error), "the normal mesh builds");
  require(mesh.vertices.size() == 3, "three vertices with normals");
  require(mesh.has_normals, "the mesh reports the normal channel");
  require(mesh.vertices[0].has_normal && mesh.vertices[0].normal[0] == 127 && mesh.vertices[0].normal[1] == 0,
          "the first normal keeps its positive bytes");
  require(mesh.vertices[1].normal[1] == -128 && mesh.vertices[1].normal[2] == -1,
          "the second normal sign-extends 0x80 and 0xFF");
  require(mesh.vertices[2].normal[2] == 64, "the third normal keeps 64");
  require(mesh.vertices[0].has_raw_position && mesh.vertices[0].raw_position[0] == -4096,
          "the raw position is the signed halfword");
  require(mesh.vertices[1].raw_position[0] == 4096 && mesh.vertices[2].raw_position[1] == 4096,
          "the raw position lanes follow the packet");
  require(mesh.vertices[0].position[0] == -1.0f && mesh.vertices[1].position[0] == 1.0f,
          "position keeps the position_scale conversion");

  // The channel scan must name the new role, and the option turns the channel off without touching the rest.
  const ChannelScan scan = scan_channels(packets.data(), packets.size(), draw);
  require(scan.channels.size() == 2 && scan.channels[1].role == ChannelRole::normals,
          "the V3-8 channel at 0x40 scans as normals");
  MeshOptions no_normals;
  no_normals.decode_normals = false;
  Mesh plain;
  require(build_mesh(packets.data(), packets.size(), draw, no_normals, plain, error), "the mesh builds without normals");
  require(!plain.has_normals && !plain.vertices[0].has_normal, "decode_normals=false skips the channel");
  require(plain.vertices.size() == 3 && plain.vertices[0].position[0] == -1.0f, "the other channels are untouched");
}

void check_render_and_png() {
  std::vector<std::uint8_t> packets = make_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packets.size()));
  // A valid measured-convention transform for the hand-authored triangle (-1,0), (1,0), (0,1): c0..c3 are the identity
  // columns, so clip = (x, y, z, 1) and every vertex is in front of the camera.
  set_identity_columns(draw);

  FrameSnapshot frame;
  frame.draws.push_back(draw);
  frame.draw_count = 1;
  frame.packets = packets;

  RenderOptions options;
  options.width = 64;
  options.height = 48;
  std::vector<std::uint8_t> rgba, mask;
  RenderStats stats;
  std::string error;
  require(render_frame(frame, options, rgba, mask, stats, error), "the frame renders");
  require(stats.shapes_drawn == 1 && stats.triangles_drawn == 1, "one shape, one triangle drawn");
  require(stats.covered_pixels > 0, "the triangle covers pixels");
  // This packet has no normal channel, so the draw keeps its base colour even though make_draw carries a light block.
  require(stats.draws_lit == 0 && stats.draws_fallback == 1 && stats.vertices_lit == 0 && stats.vertices_fallback == 3,
          "a draw without normals is a counted lighting fallback");

  const std::string path = "coregs3_fixture.png";
  require(write_png_rgba(path, options.width, options.height, rgba.data()), "the PNG is written");
  std::FILE *file = std::fopen(path.c_str(), "rb");
  require(file != nullptr, "the PNG is readable");
  std::uint8_t header[16] = {};
  require(std::fread(header, 1, sizeof header, file) == sizeof header, "the PNG header is readable");
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  require(std::memcmp(header, signature, 8) == 0, "the PNG signature");
  require(header[8] == 0 && header[9] == 0 && header[10] == 0 && header[11] == 13, "IHDR length is 13");
  require(header[12] == 'I' && header[13] == 'H' && header[14] == 'D' && header[15] == 'R', "IHDR type");
  require(size > 8 + 25, "the PNG has chunks");
  std::remove(path.c_str());
}

// The offline renderer applies the measured VU1 lighting when the draw carries the six-qword block and the mesh has
// the normal and colour channels; otherwise the vertex keeps its base colour and both the draw and the vertex are
// counted. Points are used so the pixel colour is one vertex's colour exactly, not a Gouraud interpolation.
void check_render_lighting() {
  std::vector<std::uint8_t> packets = make_lit_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packets.size()));
  set_identity_columns(draw);
  set_body_paint_block(draw);

  FrameSnapshot frame;
  frame.draws.push_back(draw);
  frame.draw_count = 1;
  frame.packets = packets;

  RenderOptions options;
  options.width = 64;
  options.height = 48;
  options.points = true;  // one pixel per vertex, so the check is the vertex colour itself
  const auto pixel = [&options](const std::vector<std::uint8_t> &rgba, int x, int y) {
    const std::size_t at = (static_cast<std::size_t>(y) * options.width + static_cast<std::size_t>(x)) * 4u;
    return std::array<std::uint8_t, 4>{rgba[at + 0], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
  };
  const std::array<std::uint8_t, 4> lit{12, 25, 6, 255};
  const std::array<std::uint8_t, 4> base{64, 128, 32, 255};
  std::vector<std::uint8_t> rgba, mask;
  RenderStats stats;
  std::string error;

  require(render_frame(frame, options, rgba, mask, stats, error), "the lit frame renders");
  require(stats.shapes_drawn == 1, "the lit shape is drawn");
  require(stats.draws_lit == 1 && stats.draws_fallback == 0, "the draw was lit");
  require(stats.vertices_lit == 3 && stats.vertices_fallback == 0, "all three vertices were lit");
  require(pixel(rgba, 16, 24) == lit, "the first vertex carries the lit colour");
  require(pixel(rgba, 48, 24) == lit, "the second vertex carries the lit colour");
  require(pixel(rgba, 32, 12) == lit, "the third vertex carries the lit colour");

  // Without the block the same geometry keeps the base colour: the fallback is counted, never shaded with a guess.
  FrameSnapshot unlit = frame;
  unlit.draws[0].items[0].light_block_qwords = 0;
  require(render_frame(unlit, options, rgba, mask, stats, error), "the fallback frame renders");
  require(stats.draws_lit == 0 && stats.draws_fallback == 1, "the draw is a counted fallback");
  require(stats.vertices_lit == 0 && stats.vertices_fallback == 3, "the vertices keep the base colour");
  require(pixel(rgba, 16, 24) == base && pixel(rgba, 48, 24) == base && pixel(rgba, 32, 12) == base,
          "the fallback pixels are the base colour");

  // An unfamiliar block variant is refused the same way.
  FrameSnapshot unknown = frame;
  unknown.draws[0].items[0].light_block_command = 0x6C09C0C9u;  // a nine-qword upload, not the six-qword form
  require(render_frame(unknown, options, rgba, mask, stats, error), "the unknown-variant frame renders");
  require(stats.draws_fallback == 1 && stats.vertices_fallback == 3, "an unknown block variant falls back");
  require(pixel(rgba, 16, 24) == base, "the unknown-variant pixel is the base colour");
}

// ---- the PGL output chain walker (format version 4) ---------------------------------------------------------------

constexpr std::uint32_t kChainBase = 0x00100000u;
constexpr std::uint32_t kChainModel0 = 0x00140000u;  // item packet addresses: matched, never read by the walker
constexpr std::uint32_t kChainModel1 = 0x00141000u;
constexpr std::uint32_t kChainBlock0 = kChainBase + 0x800u;            // six qwords
constexpr std::uint32_t kChainBlock1 = kChainBlock0 + 6u * 16u;        // nine qwords

struct ChainImage {
  std::uint32_t base = kChainBase;
  std::vector<std::uint8_t> bytes;
};

const ChainImage *g_chain_image = nullptr;

bool chain_read(std::uint32_t address, void *destination, std::size_t bytes) noexcept {
  if (g_chain_image == nullptr || address < g_chain_image->base) return false;
  const std::size_t offset = address - g_chain_image->base;
  if (offset > g_chain_image->bytes.size() || bytes > g_chain_image->bytes.size() - offset) return false;
  std::memcpy(destination, g_chain_image->bytes.data() + offset, bytes);
  return true;
}

void chain_tag(std::vector<std::uint8_t> &out, std::uint32_t word0, std::uint32_t word1, std::uint32_t word2,
               std::uint32_t word3) {
  put_u32(out, word0);
  put_u32(out, word1);
  put_u32(out, word2);
  put_u32(out, word3);
}

void chain_block(ChainImage &image, std::uint32_t address, const float (&block)[9][4], std::uint32_t qwords) {
  const std::size_t offset = address - image.base;
  if (image.bytes.size() < offset + qwords * 16u) image.bytes.resize(offset + qwords * 16u);
  for (std::uint32_t qword = 0; qword < qwords; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
      const std::uint32_t value = float_bits(block[qword][lane]);
      std::memcpy(image.bytes.data() + offset + qword * 16u + lane * 4u, &value, sizeof value);
    }
}

// The first chain exercises every tag kind func_0049D328 writes: the 0xC1 matrix packet (CNT + 8 inline qwords), two
// items, a six-qword light upload per item (REF, tag2 UNPACK into TOPS+0xC9), a 0xD9 block (CNT inline) and the
// terminating tag. The two item blocks carry different values and different qword counts so an attachment mistake fails.
void check_chain_walk() {
  const float block6[9][4] = {{1.0f, 0.0f, 0.0f, 0.0f},  {0.0f, 1.0f, 0.0f, 0.0f},  {0.0f, 0.0f, 1.0f, 0.0f},
                              {0.0f, 0.0f, 9.0f, 0.0f},  {1.0f, 1.0f, 1.0f, 1.0f},  {0.5f, 0.5f, 0.5f, 0.0f},
                              {0.0f, 0.0f, 0.0f, 0.0f},  {0.0f, 0.0f, 0.0f, 0.0f},  {0.0f, 0.0f, 0.0f, 0.0f}};
  const float block9[9][4] = {{2.0f, 0.0f, 0.0f, 0.0f},  {0.0f, 2.0f, 0.0f, 0.0f},  {0.0f, 0.0f, 2.0f, 0.0f},
                              {0.0f, 0.0f, 8.0f, 0.0f},  {0.25f, 0.25f, 0.25f, 1.0f}, {0.75f, 0.75f, 0.75f, 0.0f},
                              {3.0f, 0.0f, 0.0f, 0.0f},  {0.0f, 3.0f, 0.0f, 0.0f},  {0.0f, 0.0f, 3.0f, 0.0f}};

  ChainImage image;
  std::vector<std::uint8_t> chain;
  chain_tag(chain, 0x10000008u, 0u, 0u, 0x6C08C0C1u);  // the 0xC1 matrix packet
  for (int index = 0; index < 8; ++index) chain_tag(chain, 0u, 0u, 0u, 0u);
  chain_tag(chain, 0x30000002u, kChainModel0, 0x20000000u, 0x3F3F3F3Fu);  // item 0: STMASK + mask
  chain_tag(chain, 0x30000006u, kChainBlock0, 0u, 0x6C06C0C9u);          // item 0's six-qword light upload
  chain_tag(chain, 0x10000003u, 0u, 0u, 0x6C00C0D9u);                    // the 0xD9 block (CNT, three inline)
  chain_tag(chain, 0u, 0u, 0u, 0u);
  chain_tag(chain, 0u, 0u, 0u, 0u);
  chain_tag(chain, 0u, 0u, 0u, 0u);
  chain_tag(chain, 0x30000002u, kChainModel1, 0x0400007Bu, 0x140003A8u);  // item 1 + the pair that runs item 0
  chain_tag(chain, 0x30000009u, kChainBlock1, 0u, 0x6C09C0C9u);          // item 1's nine-qword light upload
  chain_tag(chain, 0x10000000u, 0u, 0x0400007Bu, 0x140003A8u);            // terminator: the pair that runs item 1
  image.bytes = chain;
  chain_block(image, kChainBlock0, block6, 6);
  chain_block(image, kChainBlock1, block9, 9);
  g_chain_image = &image;

  CapturedDraw draw;
  draw.item_count = 2;
  draw.items[0].address = kChainModel0;
  draw.items[0].qwords = 2;
  draw.items[1].address = kChainModel1;
  draw.items[1].qwords = 2;
  CarriedLight carried;
  ChainWalkCounters counters;
  require(walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(chain.size()), chain_read, &draw,
                            carried, counters),
          "the hand-authored chain walks");
  require(counters.complete && counters.terminator && !counters.item_mismatch, "the chain is complete");
  require(counters.tags == 7 && counters.item_tags == 2 && counters.light_tags == 2,
          "every tag is classified (7 tag qwords, 2 items, 2 light uploads)");
  require(draw.chain_valid == 1 && draw.chain_tags == 7, "the draw carries its chain summary");
  require(counters.items_uploaded == 2 && counters.items_carried == 0 && counters.items_none == 0,
          "both items carry their own upload");
  require(draw.items[0].light_source == kLightItemUploaded && draw.items[0].light_block_qwords == 6 &&
              draw.items[0].light_block_command == 0x6C06C0C9u && draw.items[0].light_block_source == kChainBlock0 &&
              draw.items[0].light_block[0][0] == float_bits(1.0f) && draw.items[0].light_block[3][2] == float_bits(9.0f),
          "item 0 gets its own six-qword block");
  require(draw.items[1].light_source == kLightItemUploaded && draw.items[1].light_block_qwords == 9 &&
              draw.items[1].light_block_command == 0x6C09C0C9u && draw.items[1].light_block_source == kChainBlock1 &&
              draw.items[1].light_block[0][0] == float_bits(2.0f) && draw.items[1].light_block[8][2] == float_bits(3.0f),
          "item 1 gets its own nine-qword block");
  require(draw.items[0].run_valid == 1 && draw.items[0].run_itop == 0x0400007Bu &&
              draw.items[0].run_mscal == 0x140003A8u && draw.items[1].run_itop == 0x0400007Bu &&
              draw.items[1].run_mscal == 0x140003A8u,
          "both run pairs are the tag2 pairs of the following tags");
  require(draw.program_valid == 1 && draw.program_uniform == 1 && counters.items_run == 2 &&
              counters.items_run_missing == 0 && counters.item_variants == 0,
          "the per-item pairs match the terminating pair");
  require(draw.light_block_qwords == 6 && draw.light_block[0][0] == float_bits(1.0f),
          "the per-draw block keeps the first item's upload");
  require(carried.runs == 2 && carried.gap_epoch == 0, "two MSCALs were executed and no parity gap was seen");
  require(carried.slot[0].valid == 1 && carried.slot[0].qwords == 6 && carried.slot[0].command == 0x6C06C0C9u &&
              carried.slot[0].block[0][0] == float_bits(1.0f),
          "the first item's upload is slot 0 (the first MSCAL's buffer)");
  require(carried.slot[1].valid == 1 && carried.slot[1].qwords == 9 && carried.slot[1].command == 0x6C09C0C9u &&
              carried.slot[1].block[0][0] == float_bits(2.0f),
          "the second item's upload is slot 1 (the double buffer's other half)");

  // The MSCAL stub immediate can differ per item while the ITOP selector does not (measured: the decomp's per-item
  // counter and 0xD9 arithmetic). The draw is still selector-uniform; the MSCAL difference is counted separately.
  std::vector<std::uint8_t> stub;
  chain_tag(stub, 0x30000001u, kChainModel0, 0x20000000u, 0x3F3F3F3Fu);
  chain_tag(stub, 0x30000001u, kChainModel1, 0x0400007Bu, 0x140003ADu);  // runs item 0 with MSCAL +5
  chain_tag(stub, 0x10000000u, 0u, 0x0400007Bu, 0x140003A8u);            // runs item 1 with the terminating pair
  image.bytes = stub;
  CapturedDraw stub_draw;
  stub_draw.item_count = 2;
  stub_draw.items[0].address = kChainModel0;
  stub_draw.items[0].qwords = 1;
  stub_draw.items[1].address = kChainModel1;
  stub_draw.items[1].qwords = 1;
  ChainWalkCounters stub_counters;
  require(walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(stub.size()), chain_read, &stub_draw,
                            carried, stub_counters),
          "the MSCAL-variant chain walks");
  require(stub_counters.complete && stub_counters.items_run == 2, "both items ran");
  require(stub_counters.item_variants == 0 && stub_counters.item_mscal_variants == 1,
          "the ITOP is uniform while the MSCAL stub varies");
  require(stub_draw.program_uniform == 1 && stub_draw.items[0].run_mscal == 0x140003ADu &&
              stub_draw.items[1].run_mscal == 0x140003A8u,
          "the per-item MSCAL words are kept and do not make the selector non-uniform");

  // A draw that uploads nothing: a single item tag and the terminator, no light tag. Its MSCAL is run 4, so it reads
  // slot 0: the six-qword block item 0 uploaded, not the most recent upload (the parity matters).
  std::vector<std::uint8_t> silent;
  chain_tag(silent, 0x30000001u, kChainModel0, 0x20000000u, 0x3F3F3F3Fu);
  chain_tag(silent, 0x10000000u, 0u, 0x0400007Bu, 0x140003A8u);
  image.bytes = silent;
  CapturedDraw carried_draw;
  carried_draw.item_count = 1;
  carried_draw.items[0].address = kChainModel0;
  carried_draw.items[0].qwords = 1;
  ChainWalkCounters carried_counters;
  require(walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(silent.size()), chain_read,
                            &carried_draw, carried, carried_counters),
          "the no-upload chain walks");
  require(carried_counters.complete && carried_counters.light_tags == 0, "no upload in the no-upload chain");
  require(carried_draw.items[0].light_source == kLightItemCarried && carried_draw.items[0].light_block_qwords == 6 &&
              carried_draw.items[0].light_block_command == 0x6C06C0C9u &&
              carried_draw.items[0].light_block[0][0] == float_bits(1.0f),
          "the no-upload item records the block of its own MSCAL parity");
  require(carried_counters.items_carried == 1 && carried_counters.items_none == 0,
          "the carried item is counted");
  require(carried_draw.items[0].run_valid == 1 && carried_draw.program_uniform == 1,
          "the no-upload item still carries its run pair");
  require(carried.runs == 5, "the no-upload chain's MSCAL advanced the parity");

  // The same chain after a parity gap: slot 0's block predates the gap and the two slots differ, so the item says so.
  carried.gap_epoch += 1;
  CapturedDraw uncertain_draw = carried_draw;
  uncertain_draw.items[0] = CapturedItem{};
  uncertain_draw.items[0].address = kChainModel0;
  uncertain_draw.items[0].qwords = 1;
  ChainWalkCounters uncertain_counters;
  require(walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(silent.size()), chain_read,
                            &uncertain_draw, carried, uncertain_counters),
          "the uncertain chain walks");
  require(uncertain_draw.items[0].light_source == kLightItemCarriedUncertain &&
              uncertain_counters.items_uncertain == 1,
          "an uncertain carried block is labelled");
  carried.gap_epoch = 0;

  // An item tag that does not match the draw's table: the chain is structurally complete but the per-item state is
  // refused (light_source stays none, chain_valid is 0), never guessed.
  std::vector<std::uint8_t> mismatched;
  chain_tag(mismatched, 0x30000001u, kChainModel1, 0x20000000u, 0x3F3F3F3Fu);
  chain_tag(mismatched, 0x10000000u, 0u, 0x0400007Bu, 0x140003A8u);
  image.bytes = mismatched;
  CapturedDraw wrong;
  wrong.item_count = 1;
  wrong.items[0].address = kChainModel0;
  wrong.items[0].qwords = 1;
  ChainWalkCounters wrong_counters;
  require(walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(mismatched.size()), chain_read, &wrong,
                            carried, wrong_counters),
          "the mismatched chain still parses structurally");
  require(wrong_counters.item_mismatch && wrong.chain_valid == 0 && wrong.items[0].light_source == kLightItemNone,
          "a mismatched item is refused, not filled from the wrong tag");

  // An unknown DMA tag id outside the PGL output chain refuses the walk.
  std::vector<std::uint8_t> unknown;
  chain_tag(unknown, 0x70000000u, 0u, 0u, 0u);
  image.bytes = unknown;
  ChainWalkCounters unknown_counters;
  CapturedDraw ignored_draw;
  ignored_draw.item_count = 0;
  require(!walk_packet_chain(kChainBase, kChainBase + static_cast<std::uint32_t>(unknown.size()), chain_read,
                             &ignored_draw, carried, unknown_counters),
          "an unknown tag id refuses the walk");
  require(unknown_counters.unknown_tags == 1 && !unknown_counters.complete, "the unknown tag is counted");

  // func_0049D328 writes nothing when the Cmd or obj->count is zero: an empty chain is complete and carries no item.
  ChainWalkCounters empty_counters;
  CapturedDraw empty_draw;
  require(walk_packet_chain(kChainBase, kChainBase, chain_read, &empty_draw, carried, empty_counters),
          "an empty chain is complete");
  require(empty_counters.complete && empty_counters.tags == 0 && empty_draw.chain_valid == 1,
          "the empty chain has no tags");
  g_chain_image = nullptr;
}

void check_mode_switch() {
  require(parse_mode(nullptr) == Mode::off, "unset is off");
  require(parse_mode("") == Mode::off, "empty is off");
  require(parse_mode("off") == Mode::off, "off");
  require(parse_mode("shadow") == Mode::shadow, "shadow");
  require(parse_mode("overlay") == Mode::overlay, "overlay");
  require(parse_mode("present") == Mode::present, "present");
  require(parse_mode("nonsense") == Mode::off, "an unknown mode parses to off and is refused by the host");
  require(std::strcmp(mode_name(Mode::shadow), "shadow") == 0, "the mode has a name");
  require(std::strcmp(mode_name(Mode::present), "present") == 0, "the live debug mode has a name");
}

// The live `present` ring (docs/COREGS3-HOOKS.md §10.3): one complete frame is published per VBlank and the renderer
// claims the newest one once; the swap moves the buffers, never copies them.
void check_present_ring() {
  present::set_active(true);
  require(present::active(), "the present ring reports what was set");

  FrameSnapshot first;
  first.vblank = 41;
  first.draw_count = 1;
  first.draws.resize(1);
  first.draws[0].sequence = 11;
  present::publish(first);
  FrameSnapshot second;
  second.vblank = 42;
  second.draw_count = 1;
  second.draws.resize(1);
  second.draws[0].sequence = 12;
  present::publish(second);

  FrameSnapshot claimed;
  require(present::claim_latest(claimed), "a published frame is claimable");
  require(claimed.vblank == 42, "the newest complete frame is the one claimed");
  require(claimed.draws.size() == 1 && claimed.draws[0].sequence == 12, "the claimed frame keeps its draws");
  require(!present::claim_latest(claimed), "a claimed frame is not returned twice");

  // The claimed buffers travel back into the ring through the swap, so the next publish needs no allocation.
  FrameSnapshot third;
  third.vblank = 43;
  third.draw_count = 0;
  present::publish(third);
  require(present::claim_latest(claimed), "a later frame is claimable");
  require(claimed.vblank == 43, "the later frame replaces the claimed one");

  present::set_active(false);
  require(!present::active(), "the present active flag clears");
}

}  // namespace

// The channels at TOPS 0x80 that the per-item lighting programs read: V3-8 normals (nine-qword/six-qword families),
// V3-32 unit float normals (the direct 0x0418 program) and the V4-8 colour (body/three-qword/passthrough). The three
// formats share a destination, so the classification must keep them distinct and must not steal the UV channels.
void check_mesh_channels_80() {
  MeshOptions options;
  options.topology = Topology::list;  // these packets carry only the channels under test, no strip table
  std::string error;

  // V3-8 at 0x80: three signed bytes per vertex, expanded by the decoder one vector per qword.
  {
    std::vector<std::uint8_t> packet;
    auto pad = [&packet] {
      while (packet.size() % 4 != 0) packet.push_back(0);
    };
    put_u32(packet, unpack_word(2, 1, 3, 0x00, true));  // V3-16 positions
    for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
      put_u16(packet, static_cast<std::uint16_t>(vertex));
      put_u16(packet, 0);
      put_u16(packet, 0);
    }
    pad();
    put_u32(packet, unpack_word(2, 2, 3, 0x80, true));  // V3-8 normals at 0x80
    const std::int8_t normal[3][3] = {{73, -51, 89}, {127, 0, 0}, {-128, 1, -1}};
    for (const auto &value : normal)
      for (std::uint32_t lane = 0; lane < 3; ++lane) packet.push_back(static_cast<std::uint8_t>(value[lane]));
    pad();
    while (packet.size() % 16 != 0) packet.push_back(0);
    CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
    Mesh mesh;
    require(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), "the normals80 mesh builds");
    require(mesh.has_normals80 && !mesh.has_colors && !mesh.has_normals, "normals80 is its own channel");
    require(mesh.vertices[0].has_normal80 && mesh.vertices[0].normal80[0] == 73 && mesh.vertices[0].normal80[1] == -51 &&
                mesh.vertices[0].normal80[2] == 89,
            "the V3-8 bytes at 0x80 sign-extend");
    require(mesh.vertices[2].normal80[0] == -128 && mesh.vertices[2].normal80[2] == -1,
            "the second V3-8 normal sign-extends 0x80 and 0xFF");
    require(mesh.vertices[0].item == 0, "the vertex names its original item");
    const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
    require(scan.channels.size() == 2 && scan.channels[1].role == ChannelRole::normals80,
            "the V3-8 channel at 0x80 scans as normals80");
  }

  // V4-8 at 0x80 stays the colour channel and does not become a normal.
  {
    std::vector<std::uint8_t> packet;
    auto pad = [&packet] {
      while (packet.size() % 4 != 0) packet.push_back(0);
    };
    put_u32(packet, unpack_word(2, 1, 3, 0x00, true));
    for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
      put_u16(packet, static_cast<std::uint16_t>(vertex));
      put_u16(packet, 0);
      put_u16(packet, 0);
    }
    pad();
    put_u32(packet, unpack_word(3, 2, 3, 0x80, true));  // V4-8 colours at 0x80
    const std::uint8_t colours[3][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};
    for (const auto &value : colours)
      for (std::uint32_t lane = 0; lane < 4; ++lane) packet.push_back(value[lane]);
    pad();
    while (packet.size() % 16 != 0) packet.push_back(0);
    CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
    Mesh mesh;
    require(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), "the colour mesh builds");
    require(mesh.has_colors && !mesh.has_normals80 && !mesh.has_normals80_float, "a V4-8 at 0x80 is a colour");
    require(mesh.vertices[1].has_color && mesh.vertices[1].color[0] == 50 && mesh.vertices[1].color[3] == 80,
            "the V4-8 colour keeps its lanes and alpha");
    const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
    require(scan.channels[1].role == ChannelRole::color, "the V4-8 channel at 0x80 scans as colour");
  }

  // V3-32 at 0x80 is the direct 0x0418 program's unit float normal.
  {
    std::vector<std::uint8_t> packet;
    auto pad = [&packet] {
      while (packet.size() % 4 != 0) packet.push_back(0);
    };
    put_u32(packet, unpack_word(2, 0, 3, 0x00, true));  // V3-32 positions
    for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
      put_u32(packet, float_bits(static_cast<float>(vertex)));
      put_u32(packet, 0);
      put_u32(packet, 0);
    }
    pad();
    put_u32(packet, unpack_word(2, 0, 3, 0x80, true));  // V3-32 normals at 0x80
    const float normals[3][3] = {{0.5f, -0.25f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (const auto &value : normals)
      for (std::uint32_t lane = 0; lane < 3; ++lane) put_u32(packet, float_bits(value[lane]));
    pad();
    while (packet.size() % 16 != 0) packet.push_back(0);
    CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
    Mesh mesh;
    require(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), "the float-normal mesh builds");
    require(mesh.has_normals80_float && !mesh.has_normals80 && !mesh.has_colors,
            "a V3-32 at 0x80 is a float normal");
    require(!mesh.vertices[0].has_raw_position, "a V3-32 position has no raw 16-bit form");
    require(mesh.vertices[0].has_normal80_float && mesh.vertices[0].normal80_float[0] == 0.5f &&
                mesh.vertices[0].normal80_float[1] == -0.25f,
            "the float normal passes through unchanged");
    const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
    require(scan.channels[1].role == ChannelRole::normals80_float,
            "the V3-32 channel at 0x80 scans as normals80_float");
  }
}

int main() {
  try {
    check_mode_switch();
    check_present_ring();
    check_capture_round_trip();
    check_chain_walk();
    check_vif_decoder();
    check_matrix();
    check_mesh();
    check_mesh_normals();
    check_mesh_channels_80();
    check_render_and_png();
    check_render_lighting();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 fixture: ok\n");
  return 0;
}
