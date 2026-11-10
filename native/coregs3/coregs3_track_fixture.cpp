// CTest native_coregs3_track: TASK-090 track M1, host only, hand-authored packets and records (no game data).
//   - the VIF decoder's measured UNPACK sign rule: bit 14 = 0 sign-extends 8/16-bit elements into the 32-bit lane,
//     bit 14 = 1 keeps them zero-widened;
//   - STMOD 2 with a STROW seed: the course position delta chain accumulates in the full 32-bit lanes (the ITOF0 input
//     is the whole lane, not the low halfword);
//   - the TOPS 0xC0 descriptor's vertex count and the position arrays that tile qword 0..count-1 (V3-8 seed, V3-16 and
//     V3-32 continuation arrays), the UV array tiled from qword 0x40 with a V2-8 segment, the colour array at 0x80 and
//     the A0 family's colour at 0xA0, and the strip table over the descriptor count;
//   - the world matrix source rule (the PGL pre-call valid/value condition, the captured packet, the carried VU0 block,
//     the last uploaded packet, and the missing/NaN refusal);
//   - the world item selection rule (main view vs mirror, no-state, the unmeasured programs);
//   - a small two-draw sky+course render through the offline rasteriser with the world options.
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vif.hpp"
#include "coregs3_world.hpp"

#include <array>
#include <cmath>
#include <cstdint>
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
void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value) { out.push_back(value); }

// A VIF UNPACK command word: opcode 0x60, VN/VL, NUM, TOPS (bit 15), USN (bit 14) and the qword destination.
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

void check_vif_sign_rule() {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 1, 2, 0x00, true, false));  // V3-16 x2, USN = 0 (sign-extend)
  put_u16(packet, 0xF000);                              // -4096
  put_u16(packet, 0x1000);                              // 4096
  put_u16(packet, 0x8000);                              // -32768
  put_u16(packet, 0x0001);                              // 1
  put_u16(packet, 0xFFFF);                              // -1
  put_u16(packet, 0x7FFF);                              // 32767
  put_u32(packet, unpack(2, 1, 1, 0x02, true, true));   // V3-16 x1, USN = 1 (zero-widen)
  put_u16(packet, 0xF000);
  put_u16(packet, 0x8000);
  put_u16(packet, 0x7FFF);
  put_u16(packet, 0);  // padding: the UNPACK payload is a whole number of 32-bit words
  const VifPacket decoded = decode_packet(packet.data(), packet.size());
  require(decoded.ok, "sign-rule packet decodes");
  std::vector<VifVector> vectors;
  require(read_vu_vectors(decoded.packed.data(), 0, 3, vectors), "sign-rule vectors readable");
  require(vectors[0].v[0] == 0xFFFFF000u, "USN 0 sign-extends -4096 to 0xFFFFF000");
  require(vectors[0].v[1] == 0x00001000u, "USN 0 keeps 4096");
  require(vectors[0].v[2] == 0xFFFF8000u, "USN 0 sign-extends -32768");
  require(vectors[1].v[0] == 0x00000001u && vectors[1].v[1] == 0xFFFFFFFFu && vectors[1].v[2] == 0x00007FFFu,
          "USN 0 sign-extends the second vector");
  require(vectors[2].v[0] == 0x0000F000u && vectors[2].v[1] == 0x00008000u && vectors[2].v[2] == 0x00007FFFu,
          "USN 1 keeps the zero-widened 16-bit value");
  require(decoded.commands[1].usn == 1 && decoded.commands[0].usn == 0, "the USN bit is exposed per command");
}

void check_mode2_accumulation() {
  std::vector<std::uint8_t> packet;
  put_stmod(packet, 2);
  put_strow(packet, 0x10000u, 0xFFFEu, 0x7FFFFFFFu, 0);
  put_u32(packet, unpack(2, 1, 2, 0x00, true, false));  // V3-16 x2 deltas, sign-extended
  put_u16(packet, 0xFFFF);  // -1
  put_u16(packet, 0x0002);  // 2
  put_u16(packet, 0x0001);  // 1
  put_u16(packet, 0x8000);  // -32768
  put_u16(packet, 0x8000);  // -32768
  put_u16(packet, 0x0000);  // 0
  const VifPacket decoded = decode_packet(packet.data(), packet.size());
  require(decoded.ok, "MODE 2 packet decodes");
  require(decoded.commands[0].mode == 2, "the position UNPACK executed with STMOD 2");
  std::vector<VifVector> vectors;
  require(read_vu_vectors(decoded.packed.data(), 0, 2, vectors), "MODE 2 vectors readable");
  // Seed + first delta, then the row accumulates the second delta.
  require(vectors[0].v[0] == 0x0000FFFFu, "seed 0x10000 + (-1)");
  require(vectors[0].v[1] == 0x00010000u, "seed 0xFFFE + 2");
  require(vectors[0].v[2] == 0x80000000u, "seed 0x7FFFFFFF + 1 wraps to 0x80000000");
  require(vectors[1].v[0] == 0x00007FFFu, "0xFFFF + (-32768) in the full 32-bit lane");
  require(vectors[1].v[1] == 0x00008000u, "0x10000 + (-32768)");
  require(vectors[1].v[2] == 0x80000000u, "the zero delta keeps the lane");
}

// The descriptor/aux packet: count 4, a STROW-seeded V3-8 commit at qword 0, V3-16 continuation at 0x01 and a V3-32
// continuation at 0x03. The UV array (V2-16 x4) tiles qword 0x40; the colour array is V4-8 x4 at 0x80; the strip table
// is one masked V1-8 write whose lane 3 carries {1 strip, 6 -> 4 vertices}.
std::vector<std::uint8_t> make_world_packet() {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));  // descriptor V3-32 x1
  put_u32(packet, 0x8000u | 4u);                        // lane0: 0x8000 | vertex count
  put_u32(packet, 0x301E4000u);                         // lane1: per-shape selector
  put_u32(packet, 0x1412u);                             // lane2: measured signature
  put_u32(packet, 0);
  put_stmod(packet, 2);
  put_strow(packet, 0x100u, 0x200u, 0x300u, 0);
  put_u32(packet, unpack(2, 2, 1, 0x00, true, true));  // V3-8 x1 seed commit, USN 1, deltas 0
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);  // padding to a word
  put_u32(packet, unpack(2, 1, 2, 0x01, true, false));  // V3-16 x2
  put_u16(packet, 10);
  put_u16(packet, 20);
  put_u16(packet, 30);
  put_u16(packet, 1);
  put_u16(packet, 2);
  put_u16(packet, 3);
  put_u32(packet, unpack(2, 0, 1, 0x03, true, false));  // V3-32 x1, raw int32 delta words
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

// The second world packet: count 4, V2-8 UV x4 and the A0 family's V4-8 colour at TOPS 0xA0.
std::vector<std::uint8_t> make_a0_packet() {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | 4u);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_stmod(packet, 2);
  put_strow(packet, 0x10u, 0x20u, 0x30u, 0);
  put_u32(packet, unpack(2, 2, 1, 0x00, true, true));
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u32(packet, unpack(2, 1, 3, 0x01, true, false));
  put_u16(packet, 0);
  put_u16(packet, 0);
  put_u16(packet, 0);
  put_u16(packet, 1);
  put_u16(packet, 1);
  put_u16(packet, 1);
  put_u16(packet, 2);
  put_u16(packet, 2);
  put_u16(packet, 2);
  put_u16(packet, 0);  // payload padding to a whole 32-bit word
  put_stmod(packet, 0);
  put_u32(packet, unpack(1, 2, 4, 0x40, true, false));  // V2-8 UV x4
  put_u8(packet, 0x10);
  put_u8(packet, 0xF0);
  put_u8(packet, 0x80);
  put_u8(packet, 0x40);
  put_u8(packet, 0x00);
  put_u8(packet, 0x00);
  put_u8(packet, 0xFF);
  put_u8(packet, 0x7F);
  put_u32(packet, unpack(3, 2, 4, 0xA0, true, true));  // V4-8 colour x4 at 0xA0
  for (std::uint8_t value = 1; value <= 16; value = static_cast<std::uint8_t>(value + 1)) put_u8(packet, value);
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));
  put_u8(packet, 1);
  put_u8(packet, 6);
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

CapturedDraw make_draw(std::uint32_t sequence, std::uint32_t pass, const std::vector<std::uint8_t> &packet,
                       std::uint32_t packet_offset) {
  CapturedDraw draw;
  draw.sequence = sequence;
  draw.pass = pass;
  draw.item_count = 1;
  draw.items[0].packet_offset = packet_offset;
  draw.items[0].packet_bytes = static_cast<std::uint32_t>(packet.size());
  draw.items[0].qwords = static_cast<std::uint16_t>((packet.size() + 15u) / 16u);
  return draw;
}

// TASK-090 world families: the measured world programs of the four course families, hand-authored packets. The
// expected values are the AOT fragments' results on the captured items (the oracle census), reduced to small cases.
std::uint32_t itop_word(std::uint32_t dispatch) { return 0x04000000u | (dispatch / 8u); }

void set_program(CapturedDraw &draw, std::uint32_t dispatch) {
  draw.items[0].run_valid = 1;
  draw.items[0].run_itop = itop_word(dispatch);
}

// Format version 6: the captured 0x00A0 factor (the measured frame-0 word is -0.50792). A blend test uses -0.5 so the
// expected values are exact in float.
void set_morph(CapturedDraw &draw, float factor) {
  std::memcpy(&draw.morph_factor, &factor, sizeof factor);
  draw.morph_factor_valid = kMorphUploadSeen;
  draw.morph_uploads = 1;
}

// 0x00A0: a 32-vertex two-key morph item, the measured layout (the first key at 0x00/0x40/0x80 covers 32 qwords, so
// the second key at 0x20/0x60/0xA0 tiles immediately after it and is bit-identical). `differ` changes one second-key
// position so the rule must refuse the item.
std::vector<std::uint8_t> make_morph_packet(bool differ, bool uv_differ = false, bool colour_differ = false) {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | 32u);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_u32(packet, unpack(2, 0, 32, 0x00, true, true));  // V3-32 x32 first key
  for (int index = 0; index < 32; ++index) {
    put_u32(packet, float_bits(1.0f + static_cast<float>(index)));
    put_u32(packet, float_bits(2.0f + static_cast<float>(index)));
    put_u32(packet, float_bits(3.0f + static_cast<float>(index)));
  }
  put_u32(packet, unpack(2, 0, 32, 0x20, true, true));  // V3-32 x32 second key
  for (int index = 0; index < 32; ++index) {
    put_u32(packet, float_bits(1.0f + static_cast<float>(index) + (differ && index == 1 ? 0.5f : 0.0f)));
    put_u32(packet, float_bits(2.0f + static_cast<float>(index)));
    put_u32(packet, float_bits(3.0f + static_cast<float>(index)));
  }
  put_u32(packet, unpack(3, 2, 32, 0x80, true, true));  // V4-8 colour x32
  for (int index = 0; index < 32; ++index) {
    put_u8(packet, static_cast<std::uint8_t>(10 + index));
    put_u8(packet, static_cast<std::uint8_t>(11 + index));
    put_u8(packet, static_cast<std::uint8_t>(12 + index));
    put_u8(packet, static_cast<std::uint8_t>(13 + index));
  }
  put_u32(packet, unpack(3, 2, 32, 0xA0, true, true));
  for (int index = 0; index < 32; ++index) {
    put_u8(packet, static_cast<std::uint8_t>(10 + index + (colour_differ && index == 1 ? 4 : 0)));
    put_u8(packet, static_cast<std::uint8_t>(11 + index));
    put_u8(packet, static_cast<std::uint8_t>(12 + index));
    put_u8(packet, static_cast<std::uint8_t>(13 + index));
  }
  put_u32(packet, unpack(1, 0, 32, 0x40, true, true));  // V2-32 UV x32 first key, tiles into 0x60
  for (int index = 0; index < 32; ++index) {
    put_u32(packet, float_bits(0.25f + 0.01f * static_cast<float>(index)));
    put_u32(packet, float_bits(0.5f + 0.01f * static_cast<float>(index)));
  }
  put_u32(packet, unpack(1, 0, 32, 0x60, true, true));  // V2-32 UV x32 second key
  for (int index = 0; index < 32; ++index) {
    put_u32(packet, float_bits(0.25f + 0.01f * static_cast<float>(index) + (uv_differ && index == 1 ? 0.5f : 0.0f)));
    put_u32(packet, float_bits(0.5f + 0.01f * static_cast<float>(index)));
  }
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));  // strip table x2
  put_u8(packet, 1);
  put_u8(packet, 90);  // 3 * (32 - 2): one strip of 32 vertices
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

// 0x0B08: V3-16 positions at 0x00, V3-8 normals at 0x80, V2-16 UVs at 0x40 and the strip table.
std::vector<std::uint8_t> make_nine_world_packet(bool with_normal) {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | 3u);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_u32(packet, unpack(2, 1, 3, 0x00, true, false));  // V3-16 x3
  const std::uint16_t positions[3][3] = {{100, 200, 300}, {110, 220, 330}, {120, 240, 360}};
  for (const auto &position : positions)
    for (std::uint16_t value : position) put_u16(packet, value);
  put_u16(packet, 0);  // payload padding to a whole 32-bit word
  if (with_normal) {
    put_u32(packet, unpack(2, 2, 3, 0x80, true, true));  // V3-8 x3
    const std::uint8_t normals[3][3] = {{127, 0, 0}, {0, 127, 0}, {0, 0, 127}};
    for (const auto &normal : normals)
      for (std::uint8_t value : normal) put_u8(packet, value);
    put_u8(packet, 0);
    put_u8(packet, 0);
    put_u8(packet, 0);
  }
  put_u32(packet, unpack(1, 1, 3, 0x40, true, false));  // V2-16 UV x3
  for (int index = 0; index < 3; ++index) {
    put_u16(packet, static_cast<std::uint16_t>(0x1000 + index));
    put_u16(packet, static_cast<std::uint16_t>(0x2000 + index));
  }
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));
  put_u8(packet, 1);
  put_u8(packet, 3);
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

// 0x1278: a MODE 2 position chain (STROW seed + V3-16 deltas) at 0x00, V3-8 normals at 0x40, V4-8 colour at 0x80.
std::vector<std::uint8_t> make_body_prepass_packet() {
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | 3u);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_stmod(packet, 2);
  put_strow(packet, 100u, 200u, 300u, 0);
  put_u32(packet, unpack(2, 2, 1, 0x00, true, true));  // V3-8 seed commit, USN 1
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u32(packet, unpack(2, 1, 2, 0x01, true, false));  // V3-16 deltas x2
  put_u16(packet, 1);
  put_u16(packet, 2);
  put_u16(packet, 3);
  put_u16(packet, 4);
  put_u16(packet, 5);
  put_u16(packet, 6);
  put_stmod(packet, 0);  // the non-position channels run without the lane accumulation
  put_u32(packet, unpack(2, 2, 3, 0x40, true, true));  // V3-8 normals at 0x40
  const std::uint8_t normals[3][3] = {{127, 0, 0}, {0, 127, 0}, {0, 0, 127}};
  for (const auto &normal : normals)
    for (std::uint8_t value : normal) put_u8(packet, value);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  const std::uint8_t body_colours[3][4] = {{50, 60, 70, 128}, {80, 90, 100, 128}, {110, 120, 130, 128}};
  put_u32(packet, unpack(3, 2, 3, 0x80, true, true));  // V4-8 colour x3
  for (const auto &colour : body_colours)
    for (std::uint8_t value : colour) put_u8(packet, value);
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));
  put_u8(packet, 1);
  put_u8(packet, 3);
  put_u8(packet, 0);
  put_u8(packet, 0);
  return packet;
}

void check_world_shape() {
  const std::vector<std::uint8_t> packet = make_world_packet();
  CapturedDraw draw = make_draw(0, kPassCourse, packet, 0);
  set_program(draw, 0x0080u);
  MeshOptions options;
  options.world_channels = true;
  Mesh mesh;
  std::string error;
  if (!build_mesh(packet.data(), packet.size(), draw, options, mesh, error))
    throw std::runtime_error("the descriptor/aux world packet builds a mesh: " + error);
  require(true, "the descriptor/aux world packet builds a mesh");
  require(mesh.has_descriptor && mesh.descriptor_vertices == 4, "the descriptor names 4 vertices");
  require(mesh.descriptor_selector == 0x301E4000u && mesh.descriptor_signature == 0x1412u,
          "the descriptor selector and signature are recorded");
  require(mesh.vertices.size() == 4, "the mesh has the descriptor's 4 vertices");
  require(mesh.vertices[0].encoding == PositionEncoding::integer_32, "MODE 2 selects the int32 lane family");
  require(mesh.vertices[0].lane_position[0] == 0x100 && mesh.vertices[0].lane_position[1] == 0x200 &&
              mesh.vertices[0].lane_position[2] == 0x300,
          "the V3-8 seed commit stores the STROW seed at qword 0");
  require(mesh.vertices[1].lane_position[0] == 0x10A && mesh.vertices[1].lane_position[1] == 0x214 &&
              mesh.vertices[1].lane_position[2] == 0x31E,
          "the V3-16 continuation adds its delta");
  require(mesh.vertices[2].lane_position[0] == 0x10B && mesh.vertices[2].lane_position[1] == 0x216 &&
              mesh.vertices[2].lane_position[2] == 0x321,
          "the second V3-16 vector accumulates");
  require(mesh.vertices[3].lane_position[0] == 0x10C && mesh.vertices[3].lane_position[1] == 0x218 &&
              mesh.vertices[3].lane_position[2] == 0x324,
          "the V3-32 continuation adds its raw int32 delta");
  require(mesh.vertices[0].has_lane_position && std::fabs(mesh.vertices[3].position[0] - 0x10C) < 1e-3f,
          "the float position mirrors the exact lane");
  require(mesh.has_uv && mesh.vertices[0].has_uv, "the UV array is read from qword 0x40");
  require(std::fabs(mesh.vertices[0].uv[0] - 1.0f) < 1e-6f && std::fabs(mesh.vertices[0].uv[1] - 2.0f) < 1e-6f,
          "V2-16 u,v are signed /4096 (0x1000, 0x2000)");
  require(std::fabs(mesh.vertices[2].uv[0] + 1.0f) < 1e-6f, "a negative V2-16 UV component");
  require(mesh.has_colors && mesh.vertices[0].color[0] == 10 && mesh.vertices[3].color[0] == 130,
          "the V4-8 colour array covers the descriptor count");
  require(mesh.strip_starts.size() == 1 && mesh.strip_lengths[0] == 4, "the strip table covers the 4 vertices");
  require(mesh.triangles() == 2, "a 4-vertex strip has two triangles");

  // The A0-family packet: V2-8 UV segments (the declared Hypothesis) and the colour at TOPS 0xA0.
  const std::vector<std::uint8_t> a0 = make_a0_packet();
  CapturedDraw a0_draw = make_draw(1, kPassCourse, a0, 0);
  set_program(a0_draw, 0x0080u);
  Mesh a0_mesh;
  if (!build_mesh(a0.data(), a0.size(), a0_draw, options, a0_mesh, error))
    throw std::runtime_error("the A0-family packet builds: " + error);
  require(a0_mesh.vertices.size() == 4 && a0_mesh.vertices[0].has_uv, "the V2-8 UV array is read");
  require(std::fabs(a0_mesh.vertices[0].uv[0] - 1.0f) < 1e-6f && std::fabs(a0_mesh.vertices[0].uv[1] + 1.0f) < 1e-6f,
          "V2-8 u,v are signed /16 (0x10 -> 1, 0xF0 -> -1)");
  require(std::fabs(a0_mesh.vertices[1].uv[0] + 8.0f) < 1e-6f && std::fabs(a0_mesh.vertices[1].uv[1] - 4.0f) < 1e-6f,
          "V2-8 0x80 -> -8 and 0x40 -> 4");
  require(a0_mesh.has_colors && a0_mesh.vertices[0].color[0] == 1 && a0_mesh.vertices[3].color[0] == 13,
          "the A0-family colour at TOPS 0xA0 is decoded");

  // The legacy (car) path must refuse the same layout rather than misread it: it has no qword-0 V3-16/V3-32 position
  // channel, so the descriptor/aux arrays stay an auxiliary channel there.
  MeshOptions legacy;
  Mesh legacy_mesh;
  require(!build_mesh(packet.data(), packet.size(), draw, legacy, legacy_mesh, error),
          "the legacy car path does not guess the world layout");
}

// TASK-090 world families: the 0x00A0 morph, 0x0B08 nine-qword and 0x1278 body pre-pass layouts, their channel rules
// and the lighting inputs they hand to coregs3_lighting. The expected colours are the formulas' AOT results on the
// captured items (the oracle census: 0x0B08 888/888, 0x1278 524/524 and 0x00A0 1358/1380 vertices exact; the A0
// remainder is the tap's zero-UV record-pairing ambiguity, not a formula difference).
void check_world_morph_family() {
  MeshOptions options;
  options.world_channels = true;
  Mesh mesh;
  std::string error;

  // 0x00A0: the two keys are equal, so the emitted geometry is the first key and the colour is the V4-8 at 0x80.
  const std::vector<std::uint8_t> morph = make_morph_packet(false);
  CapturedDraw morph_draw = make_draw(200, kPassCourse, morph, 0);
  set_program(morph_draw, 0x00A0u);
  require(world_shape_status(morph.data(), morph.size(), morph_draw.items[0]) == WorldShapeStatus::ok,
          "the equal-key 0xA0 item passes the shape rule");
  if (!build_mesh(morph.data(), morph.size(), morph_draw, options, mesh, error))
    throw std::runtime_error("the 0xA0 morph packet builds: " + error);
  require(mesh.vertices.size() == 32, "the 0xA0 item emits the descriptor's 32 vertices");
  require(mesh.vertices[0].encoding == PositionEncoding::float_32, "the 0xA0 keys are V3-32 floats");
  require(std::fabs(mesh.vertices[1].position[0] - 2.0f) < 1e-6f &&
              std::fabs(mesh.vertices[31].position[2] - 34.0f) < 1e-6f,
          "the 0xA0 geometry is the first key");
  require(mesh.vertices[0].has_color && mesh.vertices[0].color[0] == 10 && mesh.vertices[2].color[2] == 14,
          "the 0xA0 colour is the V4-8 at 0x80");
  require(mesh.has_uv && std::fabs(mesh.vertices[0].uv[0] - 0.25f) < 1e-6f &&
              std::fabs(mesh.vertices[31].uv[1] - 0.81f) < 1e-5f,
          "the 0xA0 UV is the V2-32 at 0x40");
  require(mesh.strip_lengths.size() == 1 && mesh.strip_lengths[0] == 32 && mesh.triangles() == 30,
          "the 0xA0 strip table covers the 32 vertices");
  ItemLightState morph_state;
  morph_state.program = LightProgram::morph_passthrough;
  std::array<std::uint8_t, 4> rgba{};
  require(shade_item_vertex(morph_state, mesh.vertices[0], rgba) == LightStatus::ok &&
              rgba == (std::array<std::uint8_t, 4>{10, 11, 12, 13}),
          "the 0xA0 morph emits the post-morph colour unchanged");

  // The captured A0 items have equal position/colour keys but differing UV keys: the accepted substitution uses the
  // first-key UV and the item stays selected (geometry and colour are factor-independent).
  const std::vector<std::uint8_t> uv_differ = make_morph_packet(false, true);
  CapturedDraw uv_differ_draw = make_draw(208, kPassCourse, uv_differ, 0);
  set_program(uv_differ_draw, 0x00A0u);
  require(world_shape_status(uv_differ.data(), uv_differ.size(), uv_differ_draw.items[0]) ==
              WorldShapeStatus::morph_uv_first_key,
          "differing A0 UV keys are the counted first-key substitution");
  if (!build_mesh(uv_differ.data(), uv_differ.size(), uv_differ_draw, options, mesh, error))
    throw std::runtime_error("the A0 item with differing UV keys still builds: " + error);
  require(std::fabs(mesh.vertices[0].uv[0] - 0.25f) < 1e-6f, "the A0 substitution uses the first-key UV");
  WorldItemInput uv_item;
  uv_item.pass = kPassCourse;
  uv_item.run_valid = true;
  uv_item.dispatch = 0x00A0u;
  uv_item.has_position = true;
  uv_item.shape_status = WorldShapeStatus::morph_uv_first_key;
  uv_item.state_known = true;
  uv_item.xyoffset_known = true;
  uv_item.xyoffset = kMainViewXyOffset;
  uv_item.test_known = true;
  uv_item.test = 0x0005820Cull;
  require(world_item_exclusion(uv_item) == WorldExclusion::none,
          "the counted UV substitution keeps the A0 item selected");

  // Format version 6: with the captured factor the same item is blended (ok) instead of substituted. The measured VU
  // order is key1 * (1 - s) + key2 * s; with s = -0.5 vertex 1's differing U key (0.26/0.76) becomes 0.01 and the equal
  // vertices keep their first-key value.
  CapturedDraw uv_differ_blend = uv_differ_draw;
  set_morph(uv_differ_blend, -0.5f);
  MorphKeyFacts blend_keys;
  require(world_shape_status(uv_differ.data(), uv_differ.size(), uv_differ_blend.items[0],
                             morph_blend_of(uv_differ_blend), &blend_keys) == WorldShapeStatus::ok,
          "a differing A0 UV key is blended, not refused, when the factor is captured");
  require(blend_keys.uv_keys_differ && !blend_keys.colour_keys_differ && !blend_keys.position_keys_differ,
          "the census names the differing UV key pair");
  if (!build_mesh(uv_differ.data(), uv_differ.size(), uv_differ_blend, options, mesh, error))
    throw std::runtime_error("the A0 item with a captured factor builds: " + error);
  require(std::fabs(mesh.vertices[0].uv[0] - 0.25f) < 1e-6f && std::fabs(mesh.vertices[1].uv[0] - 0.01f) < 1e-6f,
          "the A0 UV blend uses the measured operation order");

  // The 10 captured items with a differing colour key use the first-key colour and stay selected; the measured bound
  // (tap comparison) is at most 4 levels on 1-3 vertices per item.
  const std::vector<std::uint8_t> colour_differ = make_morph_packet(false, true, true);
  CapturedDraw colour_differ_draw = make_draw(209, kPassCourse, colour_differ, 0);
  set_program(colour_differ_draw, 0x00A0u);
  require(world_shape_status(colour_differ.data(), colour_differ.size(), colour_differ_draw.items[0]) ==
              WorldShapeStatus::morph_colour_first_key,
          "differing A0 colour keys are the counted first-key colour substitution");
  if (!build_mesh(colour_differ.data(), colour_differ.size(), colour_differ_draw, options, mesh, error))
    throw std::runtime_error("the A0 item with differing colour keys still builds: " + error);
  require(mesh.vertices[0].color[0] == 10 && mesh.vertices[1].color[0] == 11,
          "the A0 colour substitution uses the first-key colour");
  WorldItemInput colour_item = uv_item;
  colour_item.shape_status = WorldShapeStatus::morph_colour_first_key;
  require(world_item_exclusion(colour_item) == WorldExclusion::none,
          "the counted colour substitution keeps the A0 item selected");

  // Format version 6: the colour keys blend with the same factor. Vertex 1's R key is 11 (first) and 15 (second); with
  // s = -0.5 the VU1 formula gives 11*1.5 + 15*(-0.5) = 9, which is the tapped RGBAQ byte the fragment emits.
  CapturedDraw colour_differ_blend = colour_differ_draw;
  set_morph(colour_differ_blend, -0.5f);
  require(world_shape_status(colour_differ.data(), colour_differ.size(), colour_differ_blend.items[0],
                             morph_blend_of(colour_differ_blend), &blend_keys) == WorldShapeStatus::ok,
          "a differing A0 colour key is blended when the factor is captured");
  require(blend_keys.colour_keys_differ, "the census names the differing colour key");
  if (!build_mesh(colour_differ.data(), colour_differ.size(), colour_differ_blend, options, mesh, error))
    throw std::runtime_error("the A0 item with differing colour keys and a factor builds: " + error);
  require(mesh.vertices[0].color[0] == 10 && mesh.vertices[1].color[0] == 9,
          "the A0 colour blend uses the measured operation order and truncation");

  // A differ in the second position key is refused without a factor, never drawn on a guessed factor; with the captured
  // factor the same key pair is blended (the measured keys are equal on every capture, so this is the general rule).
  const std::vector<std::uint8_t> differ = make_morph_packet(true);
  CapturedDraw differ_draw = make_draw(201, kPassCourse, differ, 0);
  set_program(differ_draw, 0x00A0u);
  require(world_shape_status(differ.data(), differ.size(), differ_draw.items[0]) ==
              WorldShapeStatus::morph_keys_differ,
          "an unequal 0xA0 second key is refused without a factor");
  require(!build_mesh(differ.data(), differ.size(), differ_draw, options, mesh, error) &&
              error.find("morph-keys-differ") != std::string::npos,
          "build_mesh refuses the unequal 0xA0 keys with the measured reason");
  CapturedDraw differ_blend = differ_draw;
  set_morph(differ_blend, -0.5f);
  require(world_shape_status(differ.data(), differ.size(), differ_blend.items[0], morph_blend_of(differ_blend),
                             &blend_keys) == WorldShapeStatus::ok,
          "the unequal A0 position key pair is blended when the factor is captured");
  require(blend_keys.position_keys_differ, "the census names the differing position key pair");
  if (!build_mesh(differ.data(), differ.size(), differ_blend, options, mesh, error))
    throw std::runtime_error("the A0 item with unequal position keys and a factor builds: " + error);
  require(std::fabs(mesh.vertices[1].position[0] - 1.75f) < 1e-6f,
          "the A0 position blend uses key1 * (1 - s) + key2 * s");
  // The new format refusals are named, so a tool report says exactly why an item was not blended.
  require(std::strcmp(world_shape_status_name(WorldShapeStatus::morph_position_format), "morph-position-format") == 0 &&
              std::strcmp(world_shape_status_name(WorldShapeStatus::morph_uv_format), "morph-uv-format") == 0 &&
              std::strcmp(world_shape_status_name(WorldShapeStatus::morph_uv_region), "morph-uv-region") == 0,
          "the morph format refusals have their measured names");
}

// The 0x0B08 nine-qword lighting family.
void check_world_nine_family() {
  MeshOptions options;
  options.world_channels = true;
  Mesh mesh;
  std::string error;
  std::array<std::uint8_t, 4> rgba{};

  // V3-16 positions, V3-8 normals at 0x80, V2-16 UVs and the nine-qword lighting formula.
  const std::vector<std::uint8_t> nine = make_nine_world_packet(true);
  CapturedDraw nine_draw = make_draw(202, kPassCourse, nine, 0);
  set_program(nine_draw, 0x0B08u);
  require(world_shape_status(nine.data(), nine.size(), nine_draw.items[0]) == WorldShapeStatus::ok,
          "the 0xB08 item passes the shape rule");
  if (!build_mesh(nine.data(), nine.size(), nine_draw, options, mesh, error))
    throw std::runtime_error("the 0xB08 packet builds: " + error);
  require(mesh.vertices.size() == 3 && mesh.vertices[0].has_raw_position &&
              mesh.vertices[1].raw_position[0] == 110 && mesh.vertices[2].raw_position[2] == 360,
          "the 0xB08 V3-16 positions are decoded");
  require(mesh.has_normals80 && mesh.vertices[0].has_normal80 && mesh.vertices[2].normal80[2] == 127,
          "the 0xB08 V3-8 normal at 0x80 is decoded");
  require(mesh.has_uv && std::fabs(mesh.vertices[0].uv[0] - 1.0f) < 1e-6f, "the 0xB08 V2-16 UV is decoded");
  ItemLightState nine_state;
  nine_state.program = LightProgram::nine_qword_first;
  nine_state.qword[0] = {1.0f, 0.0f, 0.0f, 0.0f};
  nine_state.qword[1] = {0.0f, 1.0f, 0.0f, 0.0f};
  nine_state.qword[2] = {0.0f, 0.0f, 1.0f, 0.0f};
  nine_state.qword[3] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (std::uint32_t qword = 4; qword < 8; ++qword) nine_state.qword[qword] = {10.0f, 10.0f, 10.0f, 10.0f};
  nine_state.qword[8] = {255.0f, 255.0f, 255.0f, 128.0f};
  require(shade_item_vertex(nine_state, mesh.vertices[0], rgba) == LightStatus::ok &&
              rgba == (std::array<std::uint8_t, 4>{10, 10, 10, 10}),
          "the 0xB08 item is lit by the nine-qword first-power formula");

  // A 0xB08 item without the normal at 0x80 is refused, not drawn with a guessed normal.
  const std::vector<std::uint8_t> no_normal = make_nine_world_packet(false);
  CapturedDraw no_normal_draw = make_draw(203, kPassCourse, no_normal, 0);
  set_program(no_normal_draw, 0x0B08u);
  require(world_shape_status(no_normal.data(), no_normal.size(), no_normal_draw.items[0]) ==
              WorldShapeStatus::missing_normal80,
          "the 0xB08 item without a normal is refused");
  require(!build_mesh(no_normal.data(), no_normal.size(), no_normal_draw, options, mesh, error) &&
              error.find("missing-normal-0x80") != std::string::npos,
          "build_mesh refuses the 0xB08 item with the measured reason");
}

// The 0x1278 body/paint pre-pass family.
void check_world_body_family() {
  MeshOptions options;
  options.world_channels = true;
  Mesh mesh;
  std::string error;
  std::array<std::uint8_t, 4> rgba{};

  // The MODE 2 lane is the body formula's position; the V3-8 normal at 0x40 and the colour at 0x80 are read.
  const std::vector<std::uint8_t> body = make_body_prepass_packet();
  CapturedDraw body_draw = make_draw(204, kPassCourse, body, 0);
  set_program(body_draw, 0x1278u);
  require(world_shape_status(body.data(), body.size(), body_draw.items[0]) == WorldShapeStatus::ok,
          "the 0x1278 item passes the shape rule");
  if (!build_mesh(body.data(), body.size(), body_draw, options, mesh, error))
    throw std::runtime_error("the 0x1278 packet builds: " + error);
  require(mesh.vertices.size() == 3 && mesh.vertices[0].encoding == PositionEncoding::integer_32 &&
              mesh.vertices[0].has_lane_position,
          "the 0x1278 position family is the int32 MODE 2 lane");
  require(mesh.vertices[0].lane_position[0] == 100 && mesh.vertices[0].lane_position[1] == 200 &&
              mesh.vertices[0].lane_position[2] == 300,
          "the STROW seed is the first lane");
  require(mesh.vertices[1].lane_position[0] == 101 && mesh.vertices[2].lane_position[0] == 105 &&
              mesh.vertices[2].lane_position[2] == 309,
          "the V3-16 deltas accumulate in the 32-bit lane");
  require(!mesh.vertices[0].has_raw_position && mesh.has_normals && mesh.vertices[0].has_normal &&
              mesh.vertices[0].normal[0] == 127,
          "the 0x1278 normal at 0x40 is decoded");
  require(mesh.has_colors && mesh.vertices[0].color[0] == 50 && mesh.vertices[2].color[0] == 110,
          "the 0x1278 colour at 0x80 covers the count");
  ItemLightState body_state;
  body_state.program = LightProgram::body_prepass;
  body_state.qword[0] = {1.0f, 0.0f, 0.0f, 0.0f};
  body_state.qword[1] = {0.0f, 1.0f, 0.0f, 0.0f};
  body_state.qword[2] = {0.0f, 0.0f, 1.0f, 0.0f};
  body_state.qword[3] = {0.0f, 0.0f, 0.0f, 0.0f};  // LP
  body_state.qword[4] = {0.5f, 0.5f, 0.5f, 0.5f};  // C3
  body_state.qword[5] = {0.0f, 0.0f, 0.0f, 0.0f};  // C4
  require(shade_item_vertex(body_state, mesh.vertices[0], rgba) == LightStatus::ok &&
              rgba == (std::array<std::uint8_t, 4>{25, 30, 35, 64}),
          "the 0x1278 item is lit by the body/paint formula on the lane position");
}

// The family map, the sky's 0x1508 entry and the malformed/unsupported refusals.
void check_world_family_status() {
  // The sky's 0x1508 entry maps to the three-qword gain/clamp/bias formula.
  require(light_program_from_dispatch(0x1508u) == LightProgram::three_qword_gain,
          "the sky's 0x1508 entry maps to the three-qword gain program");
  require(light_program_from_dispatch(0x00A0u) == LightProgram::morph_passthrough &&
              light_program_from_dispatch(0x1278u) == LightProgram::body_prepass &&
              light_program_from_dispatch(0x0B08u) == LightProgram::nine_qword_first,
          "the three course families map to their measured programs");
  const std::vector<std::uint8_t> sky = make_world_packet();
  CapturedDraw sky_draw = make_draw(205, kPassCourse, sky, 0);
  set_program(sky_draw, 0x1508u);
  require(world_shape_status(sky.data(), sky.size(), sky_draw.items[0]) == WorldShapeStatus::ok,
          "the 0x1508 descriptor/position layout passes the shape rule");

  // An item of another program and one with a malformed descriptor are refused before a draw is built.
  const std::vector<std::uint8_t> morph = make_morph_packet(false);
  CapturedDraw unsupported = make_draw(206, kPassCourse, morph, 0);
  set_program(unsupported, 0x1234u);
  require(world_shape_status(morph.data(), morph.size(), unsupported.items[0]) ==
              WorldShapeStatus::unsupported_program,
          "an unmeasured world program is refused");
  std::vector<std::uint8_t> malformed = make_morph_packet(false);
  malformed[5] &= 0x7Fu;  // clear lane0 bit 15 of the descriptor (0x8003 -> 0x0003), keeping 0x1412
  CapturedDraw malformed_draw = make_draw(207, kPassCourse, malformed, 0);
  set_program(malformed_draw, 0x00A0u);
  require(world_shape_status(malformed.data(), malformed.size(), malformed_draw.items[0]) ==
              WorldShapeStatus::no_descriptor,
          "a 0xC0 vector without the descriptor flag is not a vertex count");
}

void check_world_matrix_source() {
  CapturedDraw draw;
  draw.cmd_value = float_bits(0.5f);
  for (std::uint32_t column = 0; column < 4; ++column)
    for (std::uint32_t lane = 0; lane < 4; ++lane) draw.vf[column][lane] = float_bits(2.0f);
  draw.vf[3][0] = float_bits(7.0f);
  draw.vf[3][1] = float_bits(8.0f);
  draw.vf[3][2] = float_bits(9.0f);
  draw.vf[3][3] = float_bits(1.0f);
  draw.matrix_packet_valid = 1;
  draw.matrix_packet[0][0] = 0x10000008u;  // the upload tag qword
  draw.matrix_packet[1][0] = float_bits(1.0f);
  draw.matrix_packet[2][1] = float_bits(2.0f);
  draw.matrix_packet[3][2] = float_bits(3.0f);
  draw.matrix_packet[4][0] = float_bits(4.0f);
  draw.matrix_packet[4][1] = float_bits(5.0f);
  draw.matrix_packet[4][2] = float_bits(6.0f);
  draw.matrix_packet[4][3] = float_bits(1.0f);
  // The PGL pre-call snapshot: byte 3 of pgl[0] is Obj.valid, pgl[8] is Obj.value.
  draw.pgl[0] = 0x00020000u;  // valid = 0: the upload runs
  draw.pgl[8] = float_bits(0.5f);

  WorldMatrixTracker tracker;
  VertexColumns columns;
  require(tracker.for_draw(draw, columns) == WorldMatrixSource::packet, "valid = 0 selects the captured packet");
  require(std::fabs(columns.column[0][0] - 1.0f) < 1e-6f && std::fabs(columns.column[1][1] - 2.0f) < 1e-6f &&
              std::fabs(columns.column[2][2] - 3.0f) < 1e-6f && std::fabs(columns.column[3][0] - 4.0f) < 1e-6f &&
              std::fabs(columns.column[3][3] - 1.0f) < 1e-6f,
          "the packet qwords 1..4 are c0..c3");

  CapturedDraw carried = draw;
  carried.pgl[0] = 0x01020000u;  // valid = 1, flag 0, value unchanged: no upload
  require(tracker.for_draw(carried, columns) == WorldMatrixSource::vu0, "valid + equal value keeps the VU0 block");
  require(std::fabs(columns.column[0][0] - 1.0f) < 1e-6f, "vf24 is scaled by the shape value (2 * 0.5)");
  require(std::fabs(columns.column[3][0] - 7.0f) < 1e-6f, "the translation is never scaled");

  CapturedDraw rewritten = draw;
  rewritten.pgl[0] = 0x01020001u;  // valid = 1, flag byte 1, value unchanged: VU0 changed but no upload
  require(tracker.for_draw(rewritten, columns) == WorldMatrixSource::last_packet,
          "a draw that rewrites VU0 without uploading keeps the last uploaded packet");
  require(std::fabs(columns.column[0][0] - 1.0f) < 1e-6f, "the carried packet is the previous upload");

  CapturedDraw missing_packet = draw;
  missing_packet.matrix_packet_valid = 0;
  require(tracker.for_draw(missing_packet, columns) == WorldMatrixSource::vu0,
          "an upload whose read-back is missing falls back to the counted VU0 block");

  CapturedDraw not_uploaded = draw;
  not_uploaded.matrix_packet_valid = 1;
  not_uploaded.pgl[0] = 0x01020000u;
  not_uploaded.pgl[8] = float_bits(0.25f);  // value differs: the upload runs
  require(tracker.for_draw(not_uploaded, columns) == WorldMatrixSource::packet,
          "a value change forces the upload and the packet");

  CapturedDraw nan = draw;
  nan.pgl[0] = 0x00020000u;
  nan.matrix_packet_valid = 0;
  nan.vf[0][0] = 0x7FC00000u;  // NaN
  require(tracker.for_draw(nan, columns) == WorldMatrixSource::missing, "a non-finite matrix is refused");
}

void check_world_selection() {
  WorldItemInput item;
  item.pass = kPassCourse;
  item.run_valid = true;
  item.dispatch = 0x0080u;
  item.has_position = true;
  item.state_known = true;
  item.xyoffset_known = true;
  item.xyoffset = kMainViewXyOffset;
  item.test_known = true;
  item.test = 0x0005820Cull;
  require(world_item_exclusion(item) == WorldExclusion::none, "the main-view ITOP 0x80 item is selected");

  // TASK-090 world families: the measured course families are selected when their shape rule passes.
  WorldItemInput other = item;
  other.dispatch = 0x0B08u;
  require(world_item_exclusion(other) == WorldExclusion::none, "the 0xB08 nine-qword family is selected");
  other.dispatch = 0x00A0u;
  require(world_item_exclusion(other) == WorldExclusion::none, "the 0xA0 morph family is selected");
  other.dispatch = 0x1278u;
  require(world_item_exclusion(other) == WorldExclusion::none, "the 0x1278 body pre-pass family is selected");
  other.dispatch = 0x1508u;
  require(world_item_exclusion(other) == WorldExclusion::itop_other,
          "the sky's dispatch is not a course family and is excluded in the course pass");
  WorldItemInput refused = item;
  refused.dispatch = 0x00A0u;
  refused.shape_status = WorldShapeStatus::morph_keys_differ;
  require(world_item_exclusion(refused) == WorldExclusion::shape_refused,
          "an item whose shape rule refuses it is excluded and counted");
  refused.has_position = false;
  require(world_item_exclusion(refused) == WorldExclusion::no_position,
          "a missing position keeps the no-position reason");

  WorldItemInput mirror = item;
  mirror.xyoffset = 0x0000780800007C08ull;
  require(world_item_exclusion(mirror) == WorldExclusion::mirror, "the 0x7C08/0x7808 mirror view is excluded");
  mirror.xyoffset = 0x00007C0800007C08ull;
  require(world_item_exclusion(mirror) == WorldExclusion::mirror, "the 0x7C08/0x7C08 view is excluded");

  WorldItemInput no_state = item;
  no_state.state_known = false;
  require(world_item_exclusion(no_state) == WorldExclusion::no_state, "an unpaired state is excluded");
  no_state = item;
  no_state.test = 0;
  require(world_item_exclusion(no_state) == WorldExclusion::no_state, "an all-zero TEST state is excluded");
  WorldItemInput journal_rescued = item;
  journal_rescued.test_zero = true;  // the paired record was all-zero; the pass journal must not resurrect it
  require(world_item_exclusion(journal_rescued) == WorldExclusion::no_state,
          "a zero-TEST item stays excluded after the journal state override");

  WorldItemInput no_position = item;
  no_position.has_position = false;
  require(world_item_exclusion(no_position) == WorldExclusion::no_position, "an item without a position is excluded");

  WorldItemInput sky;
  sky.pass = kPassSky;
  sky.has_position = true;
  require(world_item_exclusion(sky) == WorldExclusion::none, "a sky item is selected without program filtering");
  // TASK-090 wet reflection: the reflection pass is a world pass now; its own rule is covered by the wet-reflection
  // fixture. Here a bare reflection item (no state, no program) must not be guessed into the picture.
  WorldItemInput reflection;
  reflection.pass = kPassReflection;
  require(world_item_exclusion(reflection) == WorldExclusion::itop_other,
          "a reflection item without a measured run pair is excluded");
}

void check_world_render() {
  // A four-vertex quad: q0..q3 in the int32 lane family. The matrix maps x/8192 - 0.5, y/8192 - 0.5 to clip space,
  // so the quad covers a few pixels of a 64x48 picture.
  std::vector<std::uint8_t> packet;
  put_u32(packet, unpack(2, 0, 1, 0xC0, true, false));
  put_u32(packet, 0x8000u | 4u);
  put_u32(packet, 0x301E4000u);
  put_u32(packet, 0x1412u);
  put_u32(packet, 0);
  put_stmod(packet, 2);
  put_strow(packet, 4096u, 4096u, 0u, 0);
  put_u32(packet, unpack(2, 2, 1, 0x00, true, true));
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u8(packet, 0);
  put_u32(packet, unpack(2, 1, 3, 0x01, true, false));
  put_u16(packet, 0x0400);  // +1024
  put_u16(packet, 0);
  put_u16(packet, 0);
  put_u16(packet, 0);
  put_u16(packet, 0x0400);  // +1024 y
  put_u16(packet, 0);
  put_u16(packet, 0x0400);  // +1024 x
  put_u16(packet, 0);
  put_u16(packet, 0);
  put_u16(packet, 0);  // payload padding to a whole 32-bit word
  put_stmod(packet, 0);
  put_u32(packet, unpack(3, 2, 4, 0x80, true, true));  // white V4-8
  for (int index = 0; index < 16; ++index) put_u8(packet, 255);
  put_stmask(packet, 0x3F3F3F3Fu);
  put_u32(packet, 0x10000000u | unpack(0, 2, 2, 0x40, true, true));
  put_u8(packet, 1);
  put_u8(packet, 6);
  put_u8(packet, 0);
  put_u8(packet, 0);

  FrameSnapshot frame;
  frame.draws.push_back(make_draw(0, kPassSky, packet, 0));
  frame.draws.push_back(make_draw(1, kPassCourse, packet, 0));
  frame.packets = packet;
  const float scale = 1.0f / 8192.0f;
  for (CapturedDraw &draw : frame.draws) {
    set_program(draw, 0x0080u);
    draw.cmd_value = 0.0f;
    draw.matrix_packet_valid = 1;
    draw.matrix_packet[0][0] = 0x10000008u;
    draw.matrix_packet[1][0] = float_bits(scale);
    draw.matrix_packet[2][1] = float_bits(scale);
    draw.matrix_packet[4][0] = float_bits(-0.5f);
    draw.matrix_packet[4][1] = float_bits(-0.5f);
    draw.matrix_packet[4][3] = float_bits(1.0f);
    draw.pgl[0] = 0x00020000u;  // upload ran for both draws: the packet source
  }
  RenderOptions options;
  options.width = 64;
  options.height = 48;
  options.world_pass = true;
  std::vector<std::uint8_t> rgba, mask;
  RenderStats stats;
  std::string error;
  require(render_frame(frame, options, rgba, mask, stats, error), "the world render succeeds");
  std::uint64_t covered = 0;
  for (std::uint8_t value : mask) covered += value != 0 ? 1u : 0u;
  require(covered > 0, "the world render covers pixels");
  require(stats.world_draws == 2, "both selected world draws were rendered");
  require(stats.world_matrices_packet == 2, "both draws used the captured packet matrix");
  require(stats.world_vertices_integer_32 == 8, "both draws used the int32 lane family");
  require(stats.triangles_drawn > 0, "triangles were rasterised");
}

}  // namespace

int main() {
  try {
    check_vif_sign_rule();
    check_mode2_accumulation();
    check_world_shape();
    check_world_morph_family();
    check_world_nine_family();
    check_world_body_family();
    check_world_family_status();
    check_world_matrix_source();
    check_world_selection();
    check_world_render();
    std::printf("coregs3 track fixture: OK\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("coregs3 track fixture: %s\n", error.what());
    return 1;
  }
}
