// CTest native_coregs3: the TASK-090 phase 1 offline path, host only. No game data is read: every packet here is
// hand-authored, which is what makes these fixtures publishable.
//   - the `.gt4scene` frame block round trip, and the refusals (magic, version, truncated block, a packet range outside
//     the block, an item count above the cap);
//   - the bounded VIF decoder: an ordinary V3-16 UNPACK into the TOPS window, an absolute V4-8 UNPACK, STROW with a fill
//     cycle, and the refusals (unknown opcode, payload overrun);
//   - the channel rule: destination/format classification and the strip topology of a mesh;
//   - the composed matrix of func_0049E548 (rows 0-2 scaled by `value`, row 3 not);
//   - a PNG file: signature, IHDR size, IEND.
#include "coregs3_png.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vif.hpp"

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

CapturedDraw make_draw(std::uint32_t item_packet_bytes) {
  CapturedDraw draw;
  draw.sequence = 7;
  draw.shape = 0x00400000u;  // a guest address, never dereferenced by this fixture
  draw.cmd_value = float_bits(1.0f);
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
  return draw;
}

void check_capture_round_trip() {
  FrameSnapshot frame;
  frame.index = 3;
  frame.vblank = 2450;
  frame.camera.valid = 7;
  frame.camera.camera = 0x00401234u;
  frame.camera.pgl_block_k3[0] = 0x3F800000u;
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
  require(read.packets == frame.packets, "the packet bytes survive");
  require(read.textures[0].address == texture.address && read.textures[0].bytes[0] == 0xAB, "the texture survives");
  require(read.materials[0].address == material.address && read.materials[0].bytes[7] == 0xCD, "the material survives");
  require(read.camera.valid == 7 && read.camera.camera == 0x00401234u, "the camera record survives");

  // A block whose item points outside the packet block must be refused, not read.
  FrameSnapshot broken = frame;
  broken.draws[0].items[0].packet_offset = static_cast<std::uint32_t>(broken.packets.size()) - 4;
  std::vector<std::uint8_t> broken_block;
  require(!frame_block_bytes(broken, broken_block), "an item range outside the packet block is refused");

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
  require(positions[0].v[0] == 0x0000F000u, "the first x lane keeps the raw halfword -4096");
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
  require(matrix.row[0][0] == 1.0f && matrix.row[0][3] == 1.5f, "row 0 is scaled by the shape value");
  require(matrix.row[1][1] == 2.0f, "row 1 is scaled by the shape value");
  require(matrix.row[2][3] == -0.5f, "row 2 translation is scaled too");
  require(matrix.row[3][3] == 1.0f, "row 3 is not scaled");

  CapturedDraw zero = draw;
  zero.cmd_value = 0;
  const DrawMatrix raw = draw_matrix(zero);
  require(raw.row[0][0] == 2.0f && raw.row[0][3] == 3.0f, "a zero value uploads the raw rows");
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

void check_render_and_png() {
  std::vector<std::uint8_t> packets = make_packet();
  CapturedDraw draw = make_draw(static_cast<std::uint32_t>(packets.size()));
  draw.vf[0][0] = float_bits(4096.0f);
  draw.vf[1][1] = float_bits(4096.0f);
  draw.vf[3][0] = float_bits(0.0f);
  draw.vf[3][1] = float_bits(0.0f);
  draw.vf[3][2] = float_bits(0.0f);
  draw.vf[3][3] = float_bits(1.0f);

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

void check_mode_switch() {
  require(parse_mode(nullptr) == Mode::off, "unset is off");
  require(parse_mode("") == Mode::off, "empty is off");
  require(parse_mode("off") == Mode::off, "off");
  require(parse_mode("shadow") == Mode::shadow, "shadow");
  require(parse_mode("overlay") == Mode::overlay, "overlay");
  require(parse_mode("nonsense") == Mode::off, "an unknown mode parses to off and is refused by the host");
  require(std::strcmp(mode_name(Mode::shadow), "shadow") == 0, "the mode has a name");
}

}  // namespace

int main() {
  try {
    check_mode_switch();
    check_capture_round_trip();
    check_vif_decoder();
    check_matrix();
    check_mesh();
    check_render_and_png();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 fixture: ok\n");
  return 0;
}
