// TASK-090 CoreGS 3.0: from one captured draw's VIF packets to a mesh the offline renderer can draw.
//
// The model files are GT4's own PGLUshape data: a descriptor array plus VIF UNPACK packets of the model's channels. The
// channel roles (which UNPACK is positions, which is UV, which is colour, which is the strip reset list) are **not**
// documented by the format; docs/CHANNEL-SELECTION-AUDIT.md shows why the reference predicates cannot be adopted as a
// contract. This module therefore does two separate things:
//
//   * `scan_channels` reports every decoded UNPACK with its destination qword, format and vector count, in order. It is
//     what the offline tool prints, so a role assignment is evidence, not a guess.
//   * `build_mesh` applies one explicit, named role rule (kDefaultRoleRule below) and refuses with a message when the
//     packet does not match it. A later phase can add rules without touching the capture format.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_vif.hpp"
#include "coregs3_vertex.hpp"

namespace gt4::coregs3 {

struct CapturedItem;
struct CapturedDraw;

enum class ChannelRole : std::uint8_t {
  unknown = 0,
  descriptor,    // TOPS 0xC0 V3-32: lane0 = 0x8000 | vertex count, lane1 = the per-shape selector, lane2 = 0x1412
  positions,
  position_aux,  // TOPS 0x01..0x3F V3-8/V3-16/V3-32: the continuation arrays of the descriptor's vertex list
  uv,
  uv_aux,        // TOPS 0x41..0x7F V2-8/V2-16/V2-32: the continuation arrays of the item's UV list
  color,
  normals,       // V3-8 at TOPS 0x40: the body/paint family's normal channel
  normals80,     // V3-8 at TOPS 0x80: the nine-qword and six-qword-no-colour normal channels
  normals80_float,  // V3-32 at TOPS 0x80: the direct 0x0418 program's unit float normals
  strip_table,
  auxiliary,
};

struct ShapeChannel {
  std::uint32_t item = 0;         // item index inside the draw
  std::uint32_t command = 0;      // command index inside the decoded packet
  bool tops = false;              // packed (TOPS-relative) destination
  std::uint32_t destination = 0;  // qword address in the corresponding image
  ChannelFormat format = ChannelFormat::unknown;
  std::uint32_t vectors = 0;
  ChannelRole role = ChannelRole::unknown;
};

struct ChannelScan {
  // One entry per draw item, in item order; `decoded` is false when the item has no packet or the packet was refused.
  struct ItemInfo {
    bool decoded = false;
    bool has_descriptor = false;
    std::uint32_t descriptor_vertices = 0;
    std::uint32_t descriptor_selector = 0;
    std::uint32_t descriptor_signature = 0;
    bool has_position = false;
    bool has_aux_position = false;
    bool mode2_positions = false;    // a position UNPACK executed with STMOD 2 (the 32-bit lane accumulation)
    ChannelFormat first_position_format = ChannelFormat::unknown;
    bool has_uv = false;
    bool has_uv_aux = false;
  };
  std::vector<ShapeChannel> channels;
  std::vector<ItemInfo> items;
  std::uint32_t packets = 0;
  std::uint32_t decoded = 0;
  std::vector<std::string> errors;
};

// Classifies every UNPACK of every item packet of one draw. The classification is the default role rule; a channel it
// cannot place stays `unknown` and is still reported. `world_channels` selects the world shape layout's UV rules (V2-8
// and the continuation arrays at 0x41..0x7F count as UVs); the default false is the car pass's measured rule.
ChannelScan scan_channels(const std::uint8_t *packets, std::size_t packet_bytes, const struct CapturedDraw &draw,
                          bool world_channels = false);

// The default role rule, written out because it is a Hypothesis and must be reviewable:
//   positions    TOPS-relative qword 0x00, V3-16 or V3-32
//   normals      TOPS-relative qword 0x40, V3-8 (the car body/paint family; measured on the night-race capture)
//   normals80    TOPS-relative qword 0x80, V3-8 (the nine-qword glass/lamp and six-qword no-colour families; the VU1
//                prologue at 0x1878 converts it with ITOF0 * 1/127)
//   normals80_float  TOPS-relative qword 0x80, V3-32 (the direct ITOP 0x0418 program, which has no prologue and reads
//                unit float normals as they stand)
//   uv           TOPS-relative qword 0x40, V2-16 or V2-32 (and V3-16/V3-32, whose third component is skipped)
//   colour       TOPS-relative qword 0x80, V4-8
//   strip_table  TOPS-relative qword 0x40, V1-8 (decoded by `decode_strip_table`)
//   auxiliary    any other UNPACK (reported, not drawn)
//
// The V3-8 normal at 0x40 is the body/paint family's normal channel; the V3-8/V3-32 channels at 0x80 are the remaining
// measured programs' normals (coregs3_lighting.hpp maps each item's ITOP dispatch to its program and inputs). The
// destinations are distinct, so an item that carries both a colour and a normal at 0x80 fills both fields; the program
// that runs it reads the one its microcode uses.
//
// The V3-8 normal at 0x40 is the first channel of the lighting formula (coregs3_hooks.cpp's light block): the measured
// VU1 lighting formula is `C3 + C4 * min(0, dot(N, U))` with N the unit normal, U = (P - LP)/|P - LP|, P the model
// position after ITOF0 and C3/C4/LP from the light block at TOPS 0xC9..0xCE. MeshVertex keeps the raw signed bytes; the
// formula's consumers divide by 127 (the model stores a unit normal scaled to the byte range) and use position as
// position_scale converts it, or raw_position for the raw integer.
//
// The strip table was measured on the night-race capture (docs/COREGS3-HOOKS.md §6): its first byte is the strip
// count; each following byte is three times the triangle count of one strip, so a strip has `byte / 3 + 2` vertices
// and the strips partition the item's vertex array. A table whose first byte is 0 means "no breaks": one strip over
// the whole item, which is the all-zero variant some shapes carry. The VU1 program writes the table at VU 0xC040
// through a V1-8 UNPACK with STMASK 0x3F3F3F3F (lane 3 only), so the bytes live in lane 3 of consecutive qwords.
//
// The UV convention is measured (TASK-090 texture round 15, r13 selectors capture): a V2-16 channel carries a SIGNED
// 16-bit fixed-point fraction, u = (int16)raw_u / 4096 and v = (int16)raw_v / 4096, with NO V flip; a V2-32 (or
// V3-32) channel carries floats and passes them through unchanged. The VU1 program writes S = u * Q and T = v * Q, so
// the GS samples S/Q,T/Q = u,v: over the 95 vertices of draws 2/3/4 of the r13 tap the residual |S/Q-u|, |T/Q-v| is at
// most 3.2e-8. A V3-16/V3-32 channel uses the first two components and skips the third.
//
// MeshVertex::uv keeps those values exactly as the VU1 consumer sees them (u, v before the Q multiply): a renderer
// forms S = u * Q with the vertex's own Q, it does not rescale or flip them again.
const char *channel_role_name(ChannelRole role) noexcept;

// The role rule's strip-table test, exposed so a diagnostic tool reports exactly what build_mesh treats as the table.
bool classify_strip_table(bool tops, std::uint32_t destination, ChannelFormat format) noexcept;

// Decodes one strip-table byte list into strip vertex counts. Returns false with `error` when the list is malformed
// (empty with a nonzero count, a value that is not a multiple of three, or a total that is not `vertices`).
bool decode_strip_table(const std::vector<std::uint8_t> &bytes, std::uint32_t vertices,
                        std::vector<std::uint32_t> &lengths, std::string &error);

// Reads up to `limit` model UV pairs from one item's first UV channel at TOPS qword 0x40 (the measured V2-16/V2-32/V3-16/
// V3-32 forms, plus the V2-8 Hypothesis), for the tap's pairing. This is the same channel the item-state study uses to
// match a captured item to its tapped GIF record (the tapped S/Q,T/Q equal these u,v). Returns false when the item has
// no such channel or its packet is refused.
bool read_item_uv(const std::uint8_t *packets, std::size_t packet_bytes, const CapturedItem &item, std::size_t limit,
                  std::vector<std::array<float, 2>> &uvs);

struct MeshVertex {
  std::array<float, 3> position{};  // model-space position after MeshOptions::position_scale
  // The original draw item index this vertex came from (CapturedDraw::items). The per-item lighting programs select
  // their block and VU1 selector from this item (coregs3_lighting.hpp): a draw can mix programs, and the capture keeps
  // the light state per item, not per draw.
  std::uint32_t item = 0;
  // The raw signed 16-bit model position, before position_scale. The VU1 lighting entries read the position at TOPS+0x00
  // through ITOF0, so this is the exact integer the formulas start from; it is filled for V3-16 position channels only
  // (a V3-32 float channel sets has_raw_position = false).
  std::array<std::int32_t, 3> raw_position{};
  bool has_raw_position = false;
  // TASK-090 track M1: the full 32-bit lane of a MODE 2 position channel (the course/far_underlay family). The guest
  // model data is a delta chain: a STROW seed (the first vertex) plus signed 8/16-bit deltas accumulated in the 32-bit
  // lanes; ITOF0 converts the full lane, so nothing is truncated to the low halfword. `position` holds the same value
  // as a float and `lane_position` the exact integer, because the course matrix is only exact in the integer units.
  std::array<std::int32_t, 3> lane_position{};
  bool has_lane_position = false;
  // Which measured model position family this vertex came from (coregs3_vertex.hpp). The world renderer multiplies the
  // raw lane of an integer family and the stored float of float_32 by the captured upload columns; the car path uses
  // the metre transform. Default integer_16 keeps every legacy (no descriptor) mesh byte-identical.
  PositionEncoding encoding = PositionEncoding::integer_16;
  // The V3-8 normal channel at TOPS 0x40, sign-extended to int8 (the model value; a unit normal scaled to the byte
  // range). has_normal is false when the item carried no such channel. This is the body/paint family's normal.
  std::array<std::int8_t, 3> normal{};
  bool has_normal = false;
  // The V3-8 normal channel at TOPS 0x80 (nine-qword and six-qword-no-colour families), sign-extended to int8; and the
  // V3-32 unit float normal at the same destination (the direct 0x0418 program). At most one of the two is present in
  // the measured packets. Consumers divide the V3-8 form by 127 and use the V3-32 form as it stands.
  std::array<std::int8_t, 3> normal80{};
  bool has_normal80 = false;
  std::array<float, 3> normal80_float{};
  bool has_normal80_float = false;
  // The model UV channel: for a 16-bit channel u,v = (int16)raw / 4096 (a signed fixed-point fraction, no V flip); for a
  // 32-bit channel the raw floats, unchanged. This is the u,v the VU1 multiplies by Q to form S,T.
  std::array<float, 2> uv{};
  std::array<std::uint8_t, 4> color{255, 255, 255, 255};
  // True when the item carried a V4-8 colour at TOPS 0x80. The fallback keeps this colour; a program that reads it
  // (body, three-qword, passthrough) refuses the vertex when the flag is false instead of shading the white default.
  bool has_color = false;
  bool has_uv = false;
};

struct Mesh {
  std::vector<MeshVertex> vertices;
  std::vector<std::uint32_t> indices;  // triangle list
  std::vector<std::uint32_t> strip_starts;   // first vertex of each strip (mesh order)
  std::vector<std::uint32_t> strip_lengths;  // vertices in each strip, same order
  bool has_colors = false;
  bool has_normals = false;
  bool has_normals80 = false;
  bool has_normals80_float = false;
  bool has_uv = false;
  bool has_strip_table = false;  // at least one item carried a strip table
  // TASK-090 track M1: the item descriptor at TOPS 0xC0 (format v3-32). `descriptor_vertices` is lane0 & 0x7FFF and is
  // the item's true vertex count (the position arrays tile the VU1 memory from qword 0 to that count); `selector` is
  // lane1 (the per-shape selector) and `signature` lane2 (measured 0x1412). All three are informative, not guessed.
  bool has_descriptor = false;
  std::uint32_t descriptor_vertices = 0;
  std::uint32_t descriptor_selector = 0;
  std::uint32_t descriptor_signature = 0;
  std::uint32_t triangles() const { return static_cast<std::uint32_t>(indices.size() / 3); }
};

enum class Topology : std::uint8_t { strip = 0, list = 1, quad = 2 };

struct MeshOptions {
  // The compressed 16-bit positions are divided by this (Nenkai's reader uses 4096; docs plan §4.2 calls it a
  // Hypothesis). A caller that does not know the scale should leave it at the default and compare.
  float position_scale = 1.0f / 4096.0f;
  // Decode the V3-8 normal channel at TOPS 0x40 into MeshVertex::normal (and Mesh::has_normals). On by default; a caller
  // that only wants the original channels can turn it off and gets the same mesh as before this channel existed. The
  // 0x80 normal channels (normals80 / normals80_float) are always decoded: the per-item lighting programs read them.
  bool decode_normals = true;
  // `strip` is the proven reading (docs/COREGS3-HOOKS.md §6): the model's strip table says how many vertices each strip
  // has. `list` and `quad` are diagnostics kept for malformed or unfamiliar packets.
  Topology topology = Topology::strip;
  // Reject triangles with an edge longer than this (metres). With the strip table decoded the breaks are exact, so the
  // filter is off by default and only remains as a diagnostic for packets without a usable table. 0 disables it.
  float max_edge = 0.0f;
  // TASK-090 track M1: decode the descriptor-driven world shape layout instead of the car pass's per-channel roles.
  // When true, an item with the TOPS 0xC0 descriptor is read as `count` contiguous vectors from qword 0 (the position
  // arrays at TOPS 0x01..0x3F tile the VU1 memory up to the descriptor count), the UV array is read from qword 0x40 with
  // the same count, the colour array from 0x80 (or the A0 family), and the measured position family selects the lane
  // interpretation (integer_16 / integer_32 / float_32). False keeps the original car-pass path byte for byte: the car
  // fixtures and every car capture must decode exactly as before this task.
  bool world_channels = false;
};

// Builds one mesh from the draw's item packets. Items are concatenated in item order, which is the order the game issues
// them. Returns false with `error` when no position channel is found or a channel is inconsistent with the rule.
bool build_mesh(const std::uint8_t *packets, std::size_t packet_bytes, const struct CapturedDraw &draw,
                const MeshOptions &options, Mesh &mesh, std::string &error);

}  // namespace gt4::coregs3
