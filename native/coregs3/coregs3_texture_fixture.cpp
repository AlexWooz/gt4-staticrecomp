// CTest native_coregs3_texture: TASK-090 texture round 15. Host only, no game data: every GIF transfer and every VIF
// packet here is hand-authored, which is what makes this fixture publishable.
//
//   - the PACKED GIF decode the VU1 tap needs (coregs3_gif.hpp): RGBAQ is one byte per 32-bit lane (R at bits 0-7, G at
//     32-39 of the low qword, B at 0-7, A at 32-39 of the high qword), the Q it pairs with comes from the ST latch, ST
//     is S = low word of the low qword, T = high word of the low qword, Q = low word of the high qword, a tag with
//     NLOOP > 0 resets the latch, and the XYZ ADC bit stays the draw flag the G-TAP gate uses;
//   - the UV descriptor: U = bits 0-13, V = bits 32-45 of the low qword (the old V field, bits 16-29, is filled with a
//     different value and must not be read);
//   - descriptor 0xE (A+D): address = high qword bits 0-6, data = the low qword, recorded as a register write with no
//     vertex and no ADC side effect; gif_register_name names TEX0/CLAMP/TEX1/TEX2/TEST/ALPHA/FRAME/ZBUF;
//   - the model UV convention (coregs3_shape.hpp): V2-16 is a signed 16-bit fixed-point fraction, u = (int16)raw / 4096,
//     v = (int16)raw / 4096, with NO V flip (raw 65483 -> -0.0129 splits, 0/4096 -> 0/1); V2-32 floats pass through
//     unchanged (v is not flipped) and a V3-16 channel uses its first two components and skips the third.
#include "coregs3_gif.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vif.hpp"

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

// build_mesh reports why it refused; keep that reason in the failure instead of dropping it.
void require_mesh(bool built, const std::string &error, const char *message) {
  if (!built) throw std::runtime_error(std::string(message) + ": " + error);
}

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

void put_float(std::vector<std::uint8_t> &out, float value) { put_u32(out, float_bits(value)); }

// One PACKED GIFtag: NLOOP, EOP, a fixed PRIM, and the register list nibbles in the high qword.
void put_tag(std::vector<std::uint8_t> &out, std::uint32_t nloop, const std::vector<std::uint32_t> &registers,
             bool eop) {
  std::uint64_t lo = nloop & 0x7FFFu;
  if (eop) lo |= 1ull << 15;
  lo |= static_cast<std::uint64_t>(0x3FFu) << 47;  // PRIM, arbitrary: this fixture never draws
  lo |= static_cast<std::uint64_t>(registers.size() & 0xFu) << 60;
  std::uint64_t hi = 0;
  for (std::size_t index = 0; index < registers.size(); ++index)
    hi |= static_cast<std::uint64_t>(registers[index] & 0xFu) << (4 * index);
  put_u64(out, lo);
  put_u64(out, hi);
}

// STQ: S and T in the low qword, Q in the low word of the high qword (garbage above it).
void put_st(std::vector<std::uint8_t> &out, float s, float t, float q) {
  put_float(out, s);
  put_float(out, t);
  put_float(out, q);
  put_u32(out, 0xFFFFFFFFu);
}

// PACKED RGBA: R at low 0-7, G at low 32-39, B at high 0-7, A at high 32-39. The fields the old decode read (low bits
// 8-31, high bits 8-31) carry 0xEE so a wrong read is visible.
void put_rgba(std::vector<std::uint8_t> &out, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  const std::uint64_t lo =
      r | (0xEEull << 8) | (0xEEEEull << 16) | (static_cast<std::uint64_t>(g) << 32) | (0xEEEEEEull << 40);
  const std::uint64_t hi =
      b | (0xEEEEEEull << 8) | (static_cast<std::uint64_t>(a) << 32) | (0xEEEEEEull << 40);
  put_u64(out, lo);
  put_u64(out, hi);
}

// PACKED UV: U at bits 0-13, V at bits 32-45. Bits 16-29 (where the old decode read V) carry 0xABCD << 16.
void put_uv(std::vector<std::uint8_t> &out, std::uint32_t u, std::uint32_t v) {
  const std::uint64_t lo = (u & 0x3FFFu) | (0xABCDull << 16) | (static_cast<std::uint64_t>(v & 0x3FFFu) << 32) |
                           (0x1234ull << 48);
  put_u64(out, lo);
  put_u64(out, 0xFFFFFFFFFFFFFFFFull);
}

// PACKED XYZF2: x at low 0-15, y at low 32-47, z at high 4-27, fog at high 36-43, ADC at high bit 47.
void put_xyzf2(std::vector<std::uint8_t> &out, std::uint16_t x, std::uint16_t y, std::uint32_t z, std::uint8_t fog,
               bool adc) {
  std::uint64_t lo = x | (static_cast<std::uint64_t>(y) << 32);
  std::uint64_t hi = (static_cast<std::uint64_t>(z & 0xFFFFFFu) << 4) | (static_cast<std::uint64_t>(fog) << 36);
  if (adc) hi |= 1ull << 47;
  put_u64(out, lo);
  put_u64(out, hi);
}

// A+D (descriptor 0xE): data is the whole low qword, the address is the high qword's low 7 bits.
void put_ad(std::vector<std::uint8_t> &out, std::uint32_t address, std::uint64_t data) {
  put_u64(out, data);
  put_u64(out, (address & 0x7Fu) | (0xFFFFFFFFFFull << 7));
}

void check_packed_stream() {
  std::vector<std::uint8_t> bytes;
  put_tag(bytes, 2, {0x2, 0x1, 0x4}, true);  // ST, RGBA, XYZF2, two loops
  put_st(bytes, 0.25f, 0.5f, 1.0f / 128.0f);  // the Q latch for the RGBA that follows
  put_rgba(bytes, 0x12, 0x34, 0x56, 0x78);
  put_xyzf2(bytes, 0x0123, 0x0456, 0x123456, 0x42, false);
  put_st(bytes, -0.125f, 1.25f, 0.5f);
  put_rgba(bytes, 0x9A, 0xBC, 0xDE, 0xF0);
  put_xyzf2(bytes, 0xFF00, 0x0100, 0xABCDEF, 0x00, true);

  const GifPacket packet = parse_gif(bytes.data(), bytes.size());
  require(packet.ok, "the hand-authored PACKED transfer parses");
  require(packet.tags.size() == 1 && packet.tags[0].nloop == 2 && packet.tags[0].nreg == 3,
          "the tag reports two loops of three registers");
  require(packet.vertices.size() == 2, "two vertex kicks");

  const GifVertex &first = packet.vertices[0];
  require(first.x == 0x0123 && first.y == 0x0456, "x and y keep their packed lanes");
  require(first.z == 0x123456 && first.has_fog && first.fog == 0x42, "z and fog keep their packed lanes");
  require(!first.adc, "the first XYZF2 does not set ADC");
  require(first.r == 0x12 && first.g == 0x34 && first.b == 0x56 && first.a == 0x78,
          "RGBA is R 0-7, G 32-39 of the low qword, B 0-7, A 32-39 of the high qword");
  require(first.has_rgba && first.has_st, "the vertex carries the colour and ST state");
  require(first.q == 1.0f / 128.0f, "Q is the ST latch, not a field of the RGBA qword");
  require(first.s == 0.25f && first.t == 0.5f, "ST S and T are the two words of the low qword");

  const GifVertex &second = packet.vertices[1];
  require(second.r == 0x9A && second.g == 0xBC && second.b == 0xDE && second.a == 0xF0,
          "the second RGBA keeps its own bytes");
  require(second.q == 0.5f, "the second ST write latches its own Q");
  require(second.s == -0.125f && second.t == 1.25f, "the second ST pair is decoded from the low qword");
  require(second.adc, "the XYZF2 ADC bit at high bit 47 is still the draw flag");
  require(second.x == 0xFF00u && static_cast<std::int16_t>(second.x) == -256,
          "a negative x keeps its 12.4 halfword");
}

void check_uv_descriptor_and_q_latch() {
  std::vector<std::uint8_t> bytes;
  put_tag(bytes, 1, {0x2, 0x4}, false);  // ST then XYZF2; no EOP, the next tag continues the transfer
  put_st(bytes, 0.125f, 0.25f, 0.25f);
  put_xyzf2(bytes, 0x0010, 0x0020, 0, 0, false);
  put_tag(bytes, 1, {0x3, 0x4}, true);  // UV then XYZF2: a tag with NLOOP > 0 resets the Q latch
  put_uv(bytes, 0x1234, 0x2345);
  put_xyzf2(bytes, 0x0030, 0x0040, 0, 0, false);

  const GifPacket packet = parse_gif(bytes.data(), bytes.size());
  require(packet.ok && packet.tags.size() == 2, "the two-tag transfer parses");
  require(packet.vertices.size() == 2, "one vertex per tag");
  require(packet.vertices[0].q == 0.25f, "the ST write latched Q");
  require(packet.vertices[1].q == 1.0f, "a new tag with NLOOP > 0 resets the Q latch to 1.0");
  require(packet.vertices[1].s == 4660.0f && packet.vertices[1].t == 9029.0f,
          "UV is U bits 0-13 and V bits 32-45 of the low qword (0x1234, 0x2345)");
  require(packet.vertices[1].has_st, "the UV write provides the ST pair");
}

void check_ad_descriptor() {
  std::vector<std::uint8_t> bytes;
  put_tag(bytes, 1, {0x2, 0x1, 0x4}, false);  // one vertex, ADC clear
  put_st(bytes, 0.0f, 0.0f, 0.25f);
  put_rgba(bytes, 1, 2, 3, 4);
  put_xyzf2(bytes, 0x0100, 0x0200, 0, 0, false);
  put_tag(bytes, 2, {0xE}, true);  // A+D, two register writes
  put_ad(bytes, 0x06, 0x0123456789ABCDEFull);  // TEX0_1, bit 0 set: must NOT become ADC
  put_ad(bytes, 0x42, 0x8000000000000000ull);  // ALPHA_1

  const GifPacket packet = parse_gif(bytes.data(), bytes.size());
  require(packet.ok, "the A+D transfer parses");
  require(packet.vertices.size() == 1, "A+D kicks no vertex");
  require(!packet.vertices[0].adc, "A+D is not an ADC flag, even with data bit 0 set");
  require(packet.ad_writes.size() == 2, "both A+D writes are recorded");
  require(packet.ad_writes[0].address == 0x06u && packet.ad_writes[0].data == 0x0123456789ABCDEFull,
          "the address is high bits 0-6 and the data the whole low qword");
  require(packet.ad_writes[1].address == 0x42u && packet.ad_writes[1].data == 0x8000000000000000ull,
          "the second A+D write keeps its own address and data");

  require(std::strcmp(gif_register_name(0x06), "TEX0_1") == 0, "TEX0_1 is named");
  require(std::strcmp(gif_register_name(0x08), "CLAMP_1") == 0, "CLAMP_1 is named");
  require(std::strcmp(gif_register_name(0x14), "TEX1_1") == 0, "TEX1_1 is named");
  require(std::strcmp(gif_register_name(0x16), "TEX2_1") == 0, "TEX2_1 is named");
  require(std::strcmp(gif_register_name(0x47), "TEST_1") == 0, "TEST_1 is named");
  require(std::strcmp(gif_register_name(0x42), "ALPHA_1") == 0, "ALPHA_1 is named");
  require(std::strcmp(gif_register_name(0x4C), "FRAME_1") == 0, "FRAME_1 is named");
  require(std::strcmp(gif_register_name(0x4E), "ZBUF_1") == 0, "ZBUF_1 is named");
  require(std::strcmp(gif_register_name(0x7E), "unknown") == 0, "an unnamed register stays unknown");
}

// A VIF UNPACK command word: opcode 0x60 | vn<<26 | vl<<24, plus the TOPS bit, NUM and the destination in qwords.
// vn is components - 1; vl is 0 for 32-bit, 1 for 16-bit and 2 for 8-bit components.
std::uint32_t unpack_word(std::uint32_t vn, std::uint32_t vl, std::uint32_t num, std::uint32_t qword, bool tops) {
  return 0x60000000u | ((vn & 3u) << 26) | ((vl & 3u) << 24) | ((num & 0xFFu) << 16) | (tops ? 0x8000u : 0u) |
         (qword & 0x3FFu);
}

void pad_packet(std::vector<std::uint8_t> &packet) {
  while (packet.size() % 16 != 0) packet.push_back(0);  // the DMA REF QWC is whole qwords
}

void put_positions(std::vector<std::uint8_t> &packet) {
  put_u32(packet, unpack_word(2, 1, 3, 0x00, true));  // V3-16 positions, TOPS qword 0
  const std::int16_t positions[3][3] = {{-4096, 0, 0}, {4096, 0, 0}, {0, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad_packet(packet);
}

CapturedDraw make_draw(std::uint32_t packet_bytes) {
  CapturedDraw draw;
  draw.sequence = 1;
  draw.item_count = 1;
  draw.items[0].address = 0x00100000u;
  draw.items[0].qwords = static_cast<std::uint16_t>(packet_bytes / 16u);
  draw.items[0].event = 1u;
  draw.items[0].texture_record = 0;
  draw.items[0].material_record = 0;
  draw.items[0].packet_offset = 0;
  draw.items[0].packet_bytes = packet_bytes;
  return draw;
}

void check_uv_v2_16() {
  std::vector<std::uint8_t> packet;
  put_positions(packet);
  put_u32(packet, unpack_word(1, 1, 3, 0x40, true));  // V2-16 UVs, TOPS qword 0x40
  // Vertex 0 is the negative raw the texture research measured: 65483 as s16 is -53, /4096 = -0.012939...
  const std::uint16_t uvs[3][2] = {{65483, 4096}, {0, 4096}, {65156, 2322}};
  for (const auto &uv : uvs) {
    put_u16(packet, uv[0]);
    put_u16(packet, uv[1]);
  }
  pad_packet(packet);

  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
  MeshOptions options;
  Mesh mesh;
  std::string error;
  require_mesh(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), error, "the V2-16 UV mesh builds");
  require(mesh.vertices.size() == 3 && mesh.has_uv, "three vertices and a UV channel");
  require(mesh.vertices[0].has_uv && mesh.vertices[1].has_uv && mesh.vertices[2].has_uv,
          "every vertex carries its UV");

  require(mesh.vertices[0].uv[0] == static_cast<float>(-53) / 4096.0f,
          "raw u 65483 is the signed halfword -53, divided by 4096");
  require(mesh.vertices[0].uv[1] == 1.0f, "raw v 4096 is +1.0 (no V flip)");
  require(mesh.vertices[1].uv[0] == 0.0f && mesh.vertices[1].uv[1] == 1.0f, "the 0/4096 pair is (0, 1)");
  require(mesh.vertices[2].uv[0] == static_cast<float>(-380) / 4096.0f &&
              mesh.vertices[2].uv[1] == 2322.0f / 4096.0f,
          "the third pair keeps the signed fixed-point fraction");

  // The old convention (unsigned / 65536 with a V flip) must be ruled out explicitly.
  require(mesh.vertices[0].uv[1] != 1.0f - 4096.0f / 65536.0f, "V is not flipped");
  require(mesh.vertices[0].uv[0] != 65483.0f / 65536.0f, "u is not the unsigned 16-bit reading");

  const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
  require(scan.channels.size() == 2 && scan.channels[1].role == ChannelRole::uv && scan.channels[1].tops &&
              scan.channels[1].destination == 0x40 && scan.channels[1].format == ChannelFormat::v2_16,
          "the V2-16 channel at TOPS 0x40 scans as uv");
}

void check_uv_v2_32() {
  std::vector<std::uint8_t> packet;
  put_positions(packet);
  put_u32(packet, unpack_word(1, 0, 3, 0x40, true));  // V2-32 float UVs, TOPS qword 0x40
  const float uvs[3][2] = {{-0.25f, 1.25f}, {0.5f, -0.5f}, {2.0f, 0.0f}};
  for (const auto &uv : uvs) {
    put_float(packet, uv[0]);
    put_float(packet, uv[1]);
  }
  pad_packet(packet);

  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
  MeshOptions options;
  Mesh mesh;
  std::string error;
  require_mesh(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), error, "the V2-32 UV mesh builds");
  require(mesh.vertices.size() == 3 && mesh.has_uv, "three vertices and a UV channel");
  require(mesh.vertices[0].uv[0] == -0.25f && mesh.vertices[0].uv[1] == 1.25f,
          "32-bit UVs pass through unchanged, with no V flip");
  require(mesh.vertices[1].uv[0] == 0.5f && mesh.vertices[1].uv[1] == -0.5f, "the second float pair passes through");
  require(mesh.vertices[2].uv[0] == 2.0f && mesh.vertices[2].uv[1] == 0.0f, "the third float pair passes through");

  const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
  require(scan.channels.size() == 2 && scan.channels[1].role == ChannelRole::uv &&
              scan.channels[1].format == ChannelFormat::v2_32,
          "the V2-32 channel at TOPS 0x40 scans as uv");
}

void check_uv_v3_16() {
  std::vector<std::uint8_t> packet;
  put_positions(packet);
  put_u32(packet, unpack_word(2, 1, 3, 0x40, true));  // V3-16, TOPS qword 0x40: the third component is skipped
  const std::int16_t uvs[3][3] = {{4096, 2048, 999}, {-4096, 0, -999}, {1000, 3000, 123}};
  for (const auto &uv : uvs)
    for (std::int16_t component : uv) put_u16(packet, static_cast<std::uint16_t>(component));
  pad_packet(packet);

  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packet.size()));
  MeshOptions options;
  Mesh mesh;
  std::string error;
  require_mesh(build_mesh(packet.data(), packet.size(), draw, options, mesh, error), error, "the V3-16 UV mesh builds");
  require(mesh.vertices.size() == 3 && mesh.has_uv, "three vertices and a UV channel");
  require(mesh.vertices[0].uv[0] == 1.0f && mesh.vertices[0].uv[1] == 0.5f, "the first two signed components are UV");
  require(mesh.vertices[1].uv[0] == -1.0f && mesh.vertices[1].uv[1] == 0.0f, "the second pair keeps its signs");
  require(mesh.vertices[2].uv[0] == 1000.0f / 4096.0f && mesh.vertices[2].uv[1] == 3000.0f / 4096.0f,
          "the third pair is decoded from the same convention");

  const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
  require(scan.channels.size() == 2 && scan.channels[1].role == ChannelRole::uv &&
              scan.channels[1].format == ChannelFormat::v3_16,
          "the V3-16 channel at TOPS 0x40 scans as uv");
}

}  // namespace

int main() {
  try {
    check_packed_stream();
    check_uv_descriptor_and_q_latch();
    check_ad_descriptor();
    check_uv_v2_16();
    check_uv_v2_32();
    check_uv_v3_16();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 texture fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 texture fixture: ok\n");
  return 0;
}
