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

namespace gt4::coregs3 {

enum class ChannelRole : std::uint8_t { unknown = 0, positions, uv, color, normals, strip_table, auxiliary };

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
  std::vector<ShapeChannel> channels;
  std::uint32_t packets = 0;
  std::uint32_t decoded = 0;
  std::vector<std::string> errors;
};

// Classifies every UNPACK of every item packet of one draw. The classification is the default role rule; a channel it
// cannot place stays `unknown` and is still reported.
ChannelScan scan_channels(const std::uint8_t *packets, std::size_t packet_bytes, const struct CapturedDraw &draw);

// The default role rule, written out because it is a Hypothesis and must be reviewable:
//   positions    TOPS-relative qword 0x00, V3-16 or V3-32
//   normals      TOPS-relative qword 0x40, V3-8 (the car body/paint family; measured on the night-race capture)
//   uv           TOPS-relative qword 0x40, V2-16 or V2-32 (and V3-16/V3-32, whose third component is skipped)
//   colour       TOPS-relative qword 0x80, V4-8
//   strip_table  TOPS-relative qword 0x40, V1-8 (decoded by `decode_strip_table`)
//   auxiliary    any other UNPACK (reported, not drawn)
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
const char *channel_role_name(ChannelRole role) noexcept;

// The role rule's strip-table test, exposed so a diagnostic tool reports exactly what build_mesh treats as the table.
bool classify_strip_table(bool tops, std::uint32_t destination, ChannelFormat format) noexcept;

// Decodes one strip-table byte list into strip vertex counts. Returns false with `error` when the list is malformed
// (empty with a nonzero count, a value that is not a multiple of three, or a total that is not `vertices`).
bool decode_strip_table(const std::vector<std::uint8_t> &bytes, std::uint32_t vertices,
                        std::vector<std::uint32_t> &lengths, std::string &error);

struct MeshVertex {
  std::array<float, 3> position{};  // model-space position after MeshOptions::position_scale
  // The raw signed 16-bit model position, before position_scale. The VU1 lighting entry reads the position at TOPS+0x00
  // through ITOF0, so this is the exact integer the formula starts from; it is filled for V3-16 position channels only
  // (a V3-32 float channel sets has_raw_position = false).
  std::array<std::int32_t, 3> raw_position{};
  bool has_raw_position = false;
  // The V3-8 normal channel at TOPS 0x40, sign-extended to int8 (the model value; a unit normal scaled to the byte
  // range). has_normal is false when the item carried no such channel.
  std::array<std::int8_t, 3> normal{};
  bool has_normal = false;
  std::array<float, 2> uv{};
  std::array<std::uint8_t, 4> color{255, 255, 255, 255};
  bool has_uv = false;
};

struct Mesh {
  std::vector<MeshVertex> vertices;
  std::vector<std::uint32_t> indices;  // triangle list
  std::vector<std::uint32_t> strip_starts;   // first vertex of each strip (mesh order)
  std::vector<std::uint32_t> strip_lengths;  // vertices in each strip, same order
  bool has_colors = false;
  bool has_normals = false;
  bool has_uv = false;
  bool has_strip_table = false;  // at least one item carried a strip table
  std::uint32_t triangles() const { return static_cast<std::uint32_t>(indices.size() / 3); }
};

enum class Topology : std::uint8_t { strip = 0, list = 1, quad = 2 };

struct MeshOptions {
  // The compressed 16-bit positions are divided by this (Nenkai's reader uses 4096; docs plan §4.2 calls it a
  // Hypothesis). A caller that does not know the scale should leave it at the default and compare.
  float position_scale = 1.0f / 4096.0f;
  bool flip_v = true;  // GT4 model UVs are bottom-up; the reference reader flips V
  // Decode the V3-8 normal channel at TOPS 0x40 into MeshVertex::normal (and Mesh::has_normals). On by default; a caller
  // that only wants the original channels can turn it off and gets the same mesh as before this channel existed.
  bool decode_normals = true;
  // `strip` is the proven reading (docs/COREGS3-HOOKS.md §6): the model's strip table says how many vertices each strip
  // has. `list` and `quad` are diagnostics kept for malformed or unfamiliar packets.
  Topology topology = Topology::strip;
  // Reject triangles with an edge longer than this (metres). With the strip table decoded the breaks are exact, so the
  // filter is off by default and only remains as a diagnostic for packets without a usable table. 0 disables it.
  float max_edge = 0.0f;
};

// Builds one mesh from the draw's item packets. Items are concatenated in item order, which is the order the game issues
// them. Returns false with `error` when no position channel is found or a channel is inconsistent with the rule.
bool build_mesh(const std::uint8_t *packets, std::size_t packet_bytes, const struct CapturedDraw &draw,
                const MeshOptions &options, Mesh &mesh, std::string &error);

}  // namespace gt4::coregs3
