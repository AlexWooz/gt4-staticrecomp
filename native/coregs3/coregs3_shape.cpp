// TASK-090 CoreGS 3.0: the shape channel scan and the default role rule. See coregs3_shape.hpp.
#include "coregs3_shape.hpp"

#include "coregs3_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

bool is_position_format(ChannelFormat format) {
  return format == ChannelFormat::v3_8 || format == ChannelFormat::v3_16 || format == ChannelFormat::v3_32;
}

bool is_uv_format(ChannelFormat format) {
  return format == ChannelFormat::v2_8 || format == ChannelFormat::v2_16 || format == ChannelFormat::v2_32 ||
         format == ChannelFormat::v3_16 || format == ChannelFormat::v3_32;
}

ChannelRole classify(bool tops, std::uint32_t destination, ChannelFormat format) {
  // The descriptor every GT4 shape item carries at TOPS 0xC0 (format v3-32): lane0 = 0x8000 | vertex count, lane1 the
  // per-shape selector, lane2 the 0x1412 signature. It is data, not geometry, and names the item's vertex count.
  if (tops && destination == 0xC0u && format == ChannelFormat::v3_32) return ChannelRole::descriptor;
  if (tops && destination == 0x00u) {
    if (format == ChannelFormat::v3_16 || format == ChannelFormat::v3_32) return ChannelRole::positions;
    return ChannelRole::unknown;
  }
  // The continuation arrays of the descriptor's vertex list: the measured course items tile qword 0..count-1 with
  // V3-8/V3-16/V3-32 writes at TOPS 0x01..0x3F.
  if (tops && destination < 0x40u && is_position_format(format)) return ChannelRole::position_aux;
  // The strip table sits at TOPS-relative qword 0x40 like the UVs, distinguished by its single 8-bit component. The
  // VU1 program writes it through STMASK 0x3F3F3F3F, so only lane 3 of each qword changes; see decode_strip_table.
  if (tops && destination == 0x40u && format == ChannelFormat::v1_8) return ChannelRole::strip_table;
  // Three signed bytes at the same qword is the body/paint family's normal channel (measured; the VU1 lighting entry
  // reads it at TOPS+0x40). It is not a UV; the UV formats are the multi-byte ones below.
  if (tops && destination == 0x40u && format == ChannelFormat::v3_8) return ChannelRole::normals;
  if (tops && destination == 0x40u) {
    switch (format) {
    case ChannelFormat::v2_8:
    case ChannelFormat::v2_16:
    case ChannelFormat::v2_32:
    case ChannelFormat::v3_16:
    case ChannelFormat::v3_32: return ChannelRole::uv;
    default: return ChannelRole::unknown;
    }
  }
  // The continuation arrays of the UV list: the measured items tile the UVs across qword 0x40..0x7F with mixed forms.
  if (tops && destination > 0x40u && destination < 0x80u && is_uv_format(format)) return ChannelRole::uv_aux;
  // The remaining measured programs read their normal at TOPS 0x80: V3-8 for the nine-qword and six-qword no-colour
  // families (converted by the 0x1878 prologue), V3-32 unit floats for the direct 0x0418 program (no prologue).
  if (tops && destination == 0x80u && format == ChannelFormat::v3_8) return ChannelRole::normals80;
  if (tops && destination == 0x80u && format == ChannelFormat::v3_32) return ChannelRole::normals80_float;
  if (tops && destination == 0x80u && format == ChannelFormat::v4_8) return ChannelRole::color;
  // The A0 family stores its V4-8 colour at TOPS 0xA0 (measured; the dispatch-0xA0 program).
  if (tops && destination == 0xA0u && format == ChannelFormat::v4_8) return ChannelRole::color;
  // An absolute V3-32 vector at qword 0 is a float position channel (the wheel and tyre shapes use it).
  if (!tops && destination == 0x00u && format == ChannelFormat::v3_32) return ChannelRole::positions;
  return ChannelRole::auxiliary;
}

std::int16_t as_s16(std::uint32_t raw) { return static_cast<std::int16_t>(raw & 0xFFFFu); }
std::int8_t as_s8(std::uint32_t raw) { return static_cast<std::int8_t>(raw & 0xFFu); }
std::int32_t as_s32(std::uint32_t raw) { return static_cast<std::int32_t>(raw); }

// VIF 16-bit UNPACK stores the raw halfword; the model scale is applied by the caller's position_scale.
float decode_signed16(std::uint32_t raw) { return static_cast<float>(as_s16(raw)); }
float decode_bits(std::uint32_t raw) {
  float value = 0.0f;
  std::memcpy(&value, &raw, sizeof value);
  return value;
}

struct ItemData {
  std::uint32_t item_index = 0;      // original draw item index
  std::vector<float> positions;      // 3 floats per vertex
  std::vector<std::int32_t> raw_positions;  // 3 signed 16-bit values per vertex, V3-16 channels only
  std::vector<std::int32_t> lane_positions;  // 3 full 32-bit lanes per vertex, MODE 2 position families
  std::vector<std::int8_t> normals;  // 3 signed bytes per vertex
  std::vector<std::int8_t> normals80;  // 3 signed bytes per vertex, V3-8 at TOPS 0x80
  std::vector<float> normals80_float;  // 3 floats per vertex, V3-32 at TOPS 0x80
  std::vector<std::uint8_t> colors;  // 4 bytes per vertex
  std::vector<float> uvs;            // 2 floats per vertex
  std::vector<std::uint8_t> strip_table;
  bool has_colors = false;
  bool has_normals = false;
  bool has_normals80 = false;
  bool has_normals80_float = false;
  bool has_uv = false;
  bool raw16 = true;  // every position channel seen so far is V3-16
  PositionEncoding encoding = PositionEncoding::integer_16;
  bool has_descriptor = false;
  std::uint32_t descriptor_vertices = 0;
  std::uint32_t descriptor_selector = 0;
  std::uint32_t descriptor_signature = 0;
  std::uint32_t vertices() const { return static_cast<std::uint32_t>(positions.size() / 3); }
};

// Reads the TOPS 0xC0 descriptor of one decoded packet, if present, and fills the item's measured shape facts. The
// descriptor is a single V3-32 vector whose lanes are 0x8000|vertex_count, per-shape selector, 0x1412.
bool read_descriptor(const VifPacket &packet, ChannelScan::ItemInfo &info) {
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.destination != 0xC0u || entry.format != ChannelFormat::v3_32) continue;
    if (entry.vectors < 1) continue;
    std::vector<VifVector> vectors;
    if (!read_vu_vectors(packet.packed.data(), entry.destination * 16u, 1, vectors)) return false;
    // The measured descriptor: lane0 bit 15 (the GIF tag's EOP) marks the form, lane2 carries the 0x1412 signature. A
    // V3-32 vector at 0xC0 without them is data, not a vertex count, and must not be read as one.
    if ((vectors[0].v[0] & 0x8000u) == 0 || vectors[0].v[2] != 0x1412u) return false;
    info.has_descriptor = true;
    info.descriptor_vertices = vectors[0].v[0] & 0x7FFFu;
    info.descriptor_selector = vectors[0].v[1];
    info.descriptor_signature = vectors[0].v[2];
    return true;
  }
  return false;
}

// TASK-090 world families: the channel facts of one decoded item packet. The five measured world programs and the
// channels each reads are described in coregs3_shape.hpp (world_shape_status); this struct collects them in one scan.
struct WorldFamilyFacts {
  bool descriptor = false;
  std::uint32_t count = 0;
  bool seed = false;               // a V3-8 write at qword 0 (the STROW seed commit)
  std::uint32_t positions_at_0 = 0;   // V3-16/V3-32 vectors at qword 0
  std::uint32_t positions_at_20 = 0;  // the 0x00A0 second keyframe's position array
  std::uint32_t positions_other = 0;  // continuation arrays at qword 0x01..0x3F other than 0x20
  std::uint32_t uv_at_40 = 0;      // qword vectors the UV write at 0x40 covers
  std::uint32_t uv_at_60 = 0;      // the 0x00A0 second keyframe's UV array
  std::uint32_t colour80 = 0;
  std::uint32_t colourA0 = 0;
  std::uint32_t normal40 = 0;
  std::uint32_t normal80 = 0;
};

WorldFamilyFacts world_family_facts(const VifPacket &packet) {
  WorldFamilyFacts facts;
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops) continue;
    if (entry.destination == 0xC0u && entry.format == ChannelFormat::v3_32) {
      std::vector<VifVector> vectors;
      if (read_vu_vectors(packet.packed.data(), 0xC0u * 16u, 1, vectors) && (vectors[0].v[0] & 0x8000u) != 0 &&
          vectors[0].v[2] == 0x1412u) {
        facts.descriptor = true;
        facts.count = vectors[0].v[0] & 0x7FFFu;
      }
      continue;
    }
    if (entry.destination == 0x00u && entry.format == ChannelFormat::v3_8) {
      facts.seed = true;
      continue;
    }
    if (entry.destination < 0x40u && is_position_format(entry.format)) {
      if (entry.destination == 0x00u)
        facts.positions_at_0 += entry.vectors;
      else if (entry.destination == 0x20u)
        facts.positions_at_20 += entry.vectors;
      else
        facts.positions_other += entry.vectors;
      continue;
    }
    if (entry.destination == 0x40u && entry.format == ChannelFormat::v3_8) {
      facts.normal40 += entry.vectors;
      continue;
    }
    if (entry.destination == 0x40u && is_uv_format(entry.format)) {
      facts.uv_at_40 += entry.vectors;
      continue;
    }
    if (entry.destination == 0x60u && is_uv_format(entry.format)) {
      facts.uv_at_60 += entry.vectors;
      continue;
    }
    if (entry.destination == 0x80u && entry.format == ChannelFormat::v3_8) {
      facts.normal80 += entry.vectors;
      continue;
    }
    if (entry.destination == 0x80u && entry.format == ChannelFormat::v4_8) {
      facts.colour80 += entry.vectors;
      continue;
    }
    if (entry.destination == 0xA0u && entry.format == ChannelFormat::v4_8) facts.colourA0 += entry.vectors;
  }
  return facts;
}

// Compares the first `lanes` 32-bit lanes of `count` qwords between two destinations. The 0xA0 UV arrays share their
// destination with the masked V1-8 strip table, which overwrites lane 3 of the first qwords with the strip bytes; the
// program's UV consumers read lanes 0/1 (S,T), so the UV comparison is lane-limited. Positions compare 3 lanes and the
// V4-8 colour all 4 (each byte sits in one 32-bit lane after the UNPACK).
bool qword_lanes_equal(const VifPacket &packet, std::uint32_t first, std::uint32_t second, std::uint32_t count,
                       std::uint32_t lanes) {
  if (first + count > kVuQwords || second + count > kVuQwords) return false;
  for (std::uint32_t qword = 0; qword < count; ++qword) {
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
      std::uint32_t a = 0, b = 0;
      std::memcpy(&a, packet.packed.data() + static_cast<std::size_t>(first + qword) * 16u + lane * 4u, 4);
      std::memcpy(&b, packet.packed.data() + static_cast<std::size_t>(second + qword) * 16u + lane * 4u, 4);
      if (a != b) return false;
    }
  }
  return true;
}

// The family rule of world_shape_status, on an already decoded packet.
WorldShapeStatus world_shape_status_packet(const VifPacket &packet, std::uint32_t dispatch) {
  const WorldFamilyFacts facts = world_family_facts(packet);
  const bool supported = dispatch == 0x0080u || dispatch == 0x00A0u || dispatch == 0x0B08u ||
                         dispatch == 0x1278u || dispatch == 0x1508u;
  if (!supported) return WorldShapeStatus::unsupported_program;
  if (!facts.descriptor) return WorldShapeStatus::no_descriptor;
  const std::uint32_t count = facts.count;
  if (count == 0) return WorldShapeStatus::no_descriptor;
  // The first key's position list: the STROW seed (if any) and the position arrays tiled from qword 0. For the
  // non-morph families an aux array at 0x20 is part of that list; the morph family uses 0x20 as its second key instead.
  const std::uint32_t first_positions =
      (facts.seed ? 1u : 0u) + facts.positions_at_0 + facts.positions_at_20 + facts.positions_other;
  if (dispatch == 0x00A0u) {
    // The two-key morph: the first key at 0x00 and the second at 0x20 must both cover the output count. Their position
    // lanes must be equal (the geometry is then independent of the factor at 0xF2, which is not captured); the colour
    // and UV keys may differ and are counted substitutions.
    if (facts.positions_at_0 < count || facts.positions_at_20 < count) return WorldShapeStatus::position_count;
    if (facts.colour80 < count || facts.colourA0 < count) return WorldShapeStatus::missing_colour;
    if (facts.uv_at_40 < count || facts.uv_at_60 < count) return WorldShapeStatus::position_count;
    if (!qword_lanes_equal(packet, 0x00u, 0x20u, count, 3u)) return WorldShapeStatus::morph_keys_differ;
    // The position keys are equal, so the geometry is independent of the morph factor. A differing colour or UV key
    // means the program would blend it by the uncaptured factor; the first key is used and the substitution is counted.
    if (!qword_lanes_equal(packet, 0x80u, 0xA0u, count, 4u)) return WorldShapeStatus::morph_colour_first_key;
    if (!qword_lanes_equal(packet, 0x40u, 0x60u, count, 2u)) return WorldShapeStatus::morph_uv_first_key;
    return WorldShapeStatus::ok;
  }
  if (first_positions == 0) return WorldShapeStatus::no_position;
  if (dispatch == 0x0B08u) {
    if (first_positions < count) return WorldShapeStatus::position_count;
    if (facts.normal80 < count) return WorldShapeStatus::missing_normal80;
    return WorldShapeStatus::ok;
  }
  if (dispatch == 0x1278u) {
    if (first_positions < count) return WorldShapeStatus::position_count;
    if (facts.normal40 < count) return WorldShapeStatus::missing_normal40;
    if (facts.colour80 < count) return WorldShapeStatus::missing_colour;
    return WorldShapeStatus::ok;
  }
  // The 0x0080 main view and the sky's 0x1508: the descriptor's position list is the drawn geometry. Their lighting
  // program (pass-through / three-qword gain) reuses the V4-8 colour the shape may or may not carry; a missing colour
  // is a counted per-vertex fallback in the lighting stage, not a shape refusal (the M1 baseline never filtered on it).
  if (first_positions == 0) return WorldShapeStatus::no_position;
  return WorldShapeStatus::ok;
}

// The measured position family of an item: MODE 2 anywhere in the position region means the lanes are 32-bit
// accumulators (the STROW seed + signed deltas); otherwise the first position UNPACK's element width decides between the
// V3-32 float family and the V3-16 integer family. The result is a property of the packet, recorded in the item info.
void read_position_family(const VifPacket &packet, ChannelScan::ItemInfo &info) {
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.destination >= 0x40u || !is_position_format(entry.format)) continue;
    // The V3-8 write at qword 0 is the STROW seed commit, not a position array by itself: it stores the row (the first
    // vertex) into qword 0, and a packet that carries only this write has no vertex list to draw.
    if (entry.destination == 0 && entry.format == ChannelFormat::v3_8) continue;
    if (!info.has_position) {
      info.has_position = true;
      info.first_position_format = entry.format;
    }
    if (entry.destination != 0) info.has_aux_position = true;
    if (entry.mode == 2) info.mode2_positions = true;
  }
}

// One UV array segment: a UNPACK at qword `first` with `count` vertices of `format`, executed with `mode`.
struct UvSegment {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  ChannelFormat format = ChannelFormat::unknown;
  std::uint32_t mode = 0;
};

// Reads the UV array of a world item. The measured items tile it contiguously from qword 0x40 to 0x40+count-1, mixing
// V2-8/V2-16/V2-32 segments; each segment's own format and MODE decide the coordinate. The 16-bit convention is the
// measured signed fixed-point / 4096 (no V flip), the 32-bit convention is the stored float. The 8-bit and the MODE 2
// fixed-point forms are labeled Hypothesis in the report: the capture fits the 16/32-bit forms everywhere, and the few
// V2-8 segments are decoded as signed /16 (the 8-bit counterpart of the 16-bit 1/4096 fraction) and counted.
bool read_world_uv(const VifPacket &packet, std::uint32_t count, std::vector<float> &uvs,
                   std::vector<std::uint8_t> &hypothesis) {
  std::vector<UvSegment> segments;
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.destination < 0x40u || entry.destination >= 0x80u) continue;
    if (entry.format == ChannelFormat::v1_8) continue;  // the strip table shares qword 0x40
    if (!is_uv_format(entry.format)) continue;
    UvSegment segment;
    segment.first = entry.destination;
    segment.count = entry.vectors;
    segment.format = entry.format;
    segment.mode = entry.mode;
    segments.push_back(segment);
  }
  if (segments.empty() || count == 0) return false;
  // The measured rule: the segments tile from qword 0x40 in command order, with no gap and no overlap. A packet whose
  // segments do not tile is refused (the item keeps its colour) instead of reading a guessed coordinate.
  std::uint32_t cursor = 0x40u;
  for (const UvSegment &segment : segments) {
    if (segment.first != cursor) return false;
    cursor += segment.count;
  }
  if (cursor < 0x40u + count) return false;
  // The UV region is qwords 0x40..0x7F (64 vectors); a descriptor count above that cannot be an all-UV list.
  if (count > 0x40u) return false;
  std::vector<VifVector> vectors;
  if (!read_vu_vectors(packet.packed.data(), 0x40u * 16u, count, vectors)) return false;
  uvs.resize(static_cast<std::size_t>(count) * 2u);
  hypothesis.clear();
  std::uint32_t qword = 0x40u;
  std::size_t segment = 0;
  for (std::uint32_t vertex = 0; vertex < count; ++vertex) {
    while (segment < segments.size() && qword >= segments[segment].first + segments[segment].count) {
      qword = segments[segment].first + segments[segment].count;
      ++segment;
    }
    if (segment >= segments.size()) return false;
    const UvSegment &shape = segments[segment];
    const VifVector &vector = vectors[vertex];
    const bool floats = shape.format == ChannelFormat::v2_32 || shape.format == ChannelFormat::v3_32;
    const bool bytes = shape.format == ChannelFormat::v2_8;
    if (shape.mode == 2) {
      // Hypothesis: a MODE 2 UV segment accumulates deltas in the 32-bit lanes; the fixed-point unit is taken as 4096
      // (the 16-bit convention), and the use is counted so the report can list it.
      uvs[vertex * 2 + 0] = static_cast<float>(as_s32(vector.v[0])) / 4096.0f;
      uvs[vertex * 2 + 1] = static_cast<float>(as_s32(vector.v[1])) / 4096.0f;
      hypothesis.push_back(1);
    } else if (floats) {
      uvs[vertex * 2 + 0] = decode_bits(vector.v[0]);
      uvs[vertex * 2 + 1] = decode_bits(vector.v[1]);
    } else if (bytes) {
      // Hypothesis: signed 8-bit fixed point with 16 units per 1.0 (the size-scaled counterpart of the 16-bit 1/4096
      // fraction). No capture fit isolates it; every sampled V2-8 item is counted.
      uvs[vertex * 2 + 0] = static_cast<float>(as_s8(vector.v[0])) / 16.0f;
      uvs[vertex * 2 + 1] = static_cast<float>(as_s8(vector.v[1])) / 16.0f;
      hypothesis.push_back(1);
    } else {
      uvs[vertex * 2 + 0] = static_cast<float>(as_s16(vector.v[0])) / 4096.0f;
      uvs[vertex * 2 + 1] = static_cast<float>(as_s16(vector.v[1])) / 4096.0f;
    }
    ++qword;
  }
  return true;
}

// Reads the descriptor-driven vertex list of a world item. The position arrays written at TOPS 0x00..0x3F tile the VU1
// memory from qword 0; after the packet ran, `count` vectors at qword 0 are the item's vertices. The family decides the
// lane interpretation: MODE 2 lanes are the exact int32 accumulators (position = float(lane), lane_position = lane); a
// V3-32 family is the float as stored; a V3-16 family is the signed halfword with the caller's position_scale.
bool read_world_item(const VifPacket &packet, const ChannelScan::ItemInfo &info, std::uint32_t dispatch,
                     const MeshOptions &options, ItemData &data, std::string &error) {
  if (!info.has_position) {
    error = "no position channel";
    return false;
  }
  const WorldShapeStatus status = world_shape_status_packet(packet, dispatch);
  if (status != WorldShapeStatus::ok && status != WorldShapeStatus::morph_uv_first_key &&
      status != WorldShapeStatus::morph_colour_first_key) {
    error = world_shape_status_name(status);
    return false;
  }
  const std::uint32_t count = info.descriptor_vertices;
  if (count == 0 || count > kVuQwords) {
    error = "descriptor vertex count " + std::to_string(count) + " outside the VU1 memory";
    return false;
  }
  std::vector<VifVector> vectors;
  if (!read_vu_vectors(packet.packed.data(), 0, count, vectors)) {
    error = "position array outside the decoded VU image";
    return false;
  }
  data.has_descriptor = true;
  data.descriptor_vertices = info.descriptor_vertices;
  data.descriptor_selector = info.descriptor_selector;
  data.descriptor_signature = info.descriptor_signature;
  if (info.mode2_positions) {
    data.encoding = PositionEncoding::integer_32;
    data.raw16 = false;
    for (const VifVector &vector : vectors) {
      for (std::uint32_t lane = 0; lane < 3; ++lane) {
        const std::int32_t value = as_s32(vector.v[lane]);
        data.lane_positions.push_back(value);
        data.positions.push_back(static_cast<float>(value));
      }
    }
  } else if (info.first_position_format == ChannelFormat::v3_32) {
    data.encoding = PositionEncoding::float_32;
    data.raw16 = false;
    for (const VifVector &vector : vectors)
      for (std::uint32_t lane = 0; lane < 3; ++lane) data.positions.push_back(decode_bits(vector.v[lane]));
  } else {
    data.encoding = PositionEncoding::integer_16;
    for (const VifVector &vector : vectors) {
      for (std::uint32_t lane = 0; lane < 3; ++lane) {
        const std::int32_t raw = as_s16(vector.v[lane]);
        data.positions.push_back(static_cast<float>(raw) * options.position_scale);
        data.raw_positions.push_back(raw);
      }
    }
  }

  // UVs: the item's UV array, read with the same vertex count when the segments tile.
  std::vector<std::uint8_t> uv_hypothesis;
  data.has_uv = read_world_uv(packet, count, data.uvs, uv_hypothesis);

  // Colours: the V4-8 array at TOPS 0x80 (the measured families) or the A0 family's array at TOPS 0xA0. The array is
  // contiguous from its first qword and covers the descriptor count (measured: 1,269/1,284 course items; the 15
  // without a colour at 0x80 belong to the excluded A0/other programs and are counted by the tool).
  std::uint32_t color_start = 0xFFFFFFFFu;
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.format != ChannelFormat::v4_8) continue;
    if (entry.destination != 0x80u && entry.destination != 0xA0u) continue;
    if (color_start == 0xFFFFFFFFu || entry.destination < color_start) color_start = entry.destination;
  }
  if (color_start != 0xFFFFFFFFu && count <= kVuQwords - color_start) {
    if (read_vu_vectors(packet.packed.data(), color_start * 16u, count, vectors)) {
      data.has_colors = true;
      for (const VifVector &vector : vectors)
        for (std::uint32_t lane = 0; lane < 4; ++lane) data.colors.push_back(static_cast<std::uint8_t>(vector.v[lane] & 0xFFu));
    }
  }

  // Normals: keep the car path's per-channel rule (it is the measured lighting input), decoded with each channel's own
  // vector count. A world item's passthrough program does not read them; the fields are filled for completeness.
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack) continue;
    const ChannelRole role = classify(entry.tops, entry.destination, entry.format);
    if (role != ChannelRole::normals && role != ChannelRole::normals80 && role != ChannelRole::normals80_float)
      continue;
    const std::uint8_t *image = entry.tops ? packet.packed.data() : packet.window.data();
    std::vector<VifVector> channel;
    if (!read_vu_vectors(image, entry.destination * 16u, entry.vectors, channel)) continue;
    if (role == ChannelRole::normals) {
      data.has_normals = true;
      for (const VifVector &vector : channel)
        for (std::uint32_t lane = 0; lane < 3; ++lane)
          data.normals.push_back(static_cast<std::int8_t>(vector.v[lane] & 0xFFu));
    } else if (role == ChannelRole::normals80) {
      data.has_normals80 = true;
      for (const VifVector &vector : channel)
        for (std::uint32_t lane = 0; lane < 3; ++lane)
          data.normals80.push_back(static_cast<std::int8_t>(vector.v[lane] & 0xFFu));
    } else {
      data.has_normals80_float = true;
      for (const VifVector &vector : channel)
        for (std::uint32_t lane = 0; lane < 3; ++lane) data.normals80_float.push_back(decode_bits(vector.v[lane]));
    }
  }

  // The strip table: as in the car path, the last V1-8 write at qword 0x40 wins and lane 3 carries the byte (the VU1
  // program writes it with STMASK 0x3F3F3F3F).
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.destination != 0x40u || entry.format != ChannelFormat::v1_8) continue;
    std::vector<VifVector> channel;
    if (!read_vu_vectors(packet.packed.data(), entry.destination * 16u, entry.vectors, channel)) continue;
    data.strip_table.clear();
    for (const VifVector &vector : channel)
      data.strip_table.push_back(static_cast<std::uint8_t>(vector.v[3] & 0xFFu));
  }
  return data.vertices() != 0;
}

}  // namespace

const char *world_shape_status_name(WorldShapeStatus status) noexcept {
  switch (status) {
  case WorldShapeStatus::ok: return "ok";
  case WorldShapeStatus::no_descriptor: return "no-descriptor";
  case WorldShapeStatus::no_position: return "no-position";
  case WorldShapeStatus::unsupported_program: return "unsupported-program";
  case WorldShapeStatus::morph_keys_differ: return "morph-keys-differ";
  case WorldShapeStatus::morph_uv_first_key: return "morph-uv-first-key";
  case WorldShapeStatus::morph_colour_first_key: return "morph-colour-first-key";
  case WorldShapeStatus::position_count: return "position-count";
  case WorldShapeStatus::missing_normal40: return "missing-normal-0x40";
  case WorldShapeStatus::missing_normal80: return "missing-normal-0x80";
  case WorldShapeStatus::missing_colour: return "missing-colour";
  }
  return "unknown";
}

WorldShapeStatus world_shape_status(const std::uint8_t *packets, std::size_t packet_bytes,
                                    const CapturedItem &item) {
  if (item.packet_bytes == 0) return WorldShapeStatus::unsupported_program;
  if (item.packet_offset > packet_bytes || item.packet_bytes > packet_bytes - item.packet_offset)
    return WorldShapeStatus::unsupported_program;
  const VifPacket packet = decode_packet(packets + item.packet_offset, item.packet_bytes);
  if (!packet.ok) return WorldShapeStatus::unsupported_program;
  const std::uint32_t dispatch = item.run_valid != 0 ? (item.run_itop & 0x3FFu) * 8u : 0u;
  return world_shape_status_packet(packet, dispatch);
}

const char *channel_role_name(ChannelRole role) noexcept {
  switch (role) {
  case ChannelRole::descriptor: return "descriptor";
  case ChannelRole::positions: return "positions";
  case ChannelRole::position_aux: return "position_aux";
  case ChannelRole::uv: return "uv";
  case ChannelRole::uv_aux: return "uv_aux";
  case ChannelRole::color: return "color";
  case ChannelRole::normals: return "normals";
  case ChannelRole::normals80: return "normals80";
  case ChannelRole::normals80_float: return "normals80_float";
  case ChannelRole::strip_table: return "strip_table";
  case ChannelRole::auxiliary: return "auxiliary";
  default: return "unknown";
  }
}

bool classify_strip_table(bool tops, std::uint32_t destination, ChannelFormat format) noexcept {
  return classify(tops, destination, format) == ChannelRole::strip_table;
}

bool decode_strip_table(const std::vector<std::uint8_t> &bytes, std::uint32_t vertices,
                        std::vector<std::uint32_t> &lengths, std::string &error) {
  lengths.clear();
  if (bytes.empty() || bytes[0] == 0) {
    // The all-zero variant: no strip list, so the item is one strip.
    lengths.push_back(vertices);
    return vertices != 0;
  }
  const std::uint32_t strips = bytes[0];
  if (bytes.size() < 1u + strips) {
    error = "strip table declares " + std::to_string(strips) + " strips but carries " +
            std::to_string(bytes.size() - 1u) + " bytes";
    return false;
  }
  std::uint32_t total = 0;
  for (std::uint32_t index = 0; index < strips; ++index) {
    const std::uint32_t value = bytes[1u + index];
    if (value % 3u != 0) {
      error = "strip " + std::to_string(index) + " has a length byte " + std::to_string(value) +
              " that is not a multiple of three";
      lengths.clear();
      return false;
    }
    const std::uint32_t count = value / 3u + 2u;
    lengths.push_back(count);
    total += count;
  }
  if (total != vertices) {
    error = "strip table covers " + std::to_string(total) + " vertices, the item has " + std::to_string(vertices);
    lengths.clear();
    return false;
  }
  return true;
}

bool read_item_uv(const std::uint8_t *packets, std::size_t packet_bytes, const CapturedItem &item, std::size_t limit,
                  std::vector<std::array<float, 2>> &uvs) {
  uvs.clear();
  if (item.packet_bytes == 0) return false;
  if (item.packet_offset > packet_bytes || item.packet_bytes > packet_bytes - item.packet_offset) return false;
  const VifPacket packet = decode_packet(packets + item.packet_offset, item.packet_bytes);
  if (!packet.ok) return false;
  for (const VifCommand &entry : packet.commands) {
    if (!entry.unpack || !entry.tops || entry.destination != 0x40u || !is_uv_format(entry.format)) continue;
    std::vector<VifVector> vectors;
    const std::size_t count = std::min<std::size_t>(entry.vectors, limit);
    if (!read_vu_vectors(packet.packed.data(), 0x40u * 16u, static_cast<std::uint32_t>(count), vectors)) return false;
    uvs.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const bool floats = entry.format == ChannelFormat::v2_32 || entry.format == ChannelFormat::v3_32;
      if (entry.format == ChannelFormat::v2_8) {
        uvs[index] = {static_cast<float>(as_s8(vectors[index].v[0])) / 16.0f,
                      static_cast<float>(as_s8(vectors[index].v[1])) / 16.0f};
      } else {
        uvs[index] = {floats ? decode_bits(vectors[index].v[0]) : decode_signed16(vectors[index].v[0]) / 4096.0f,
                      floats ? decode_bits(vectors[index].v[1]) : decode_signed16(vectors[index].v[1]) / 4096.0f};
      }
    }
    return !uvs.empty();
  }
  return false;
}

ChannelScan scan_channels(const std::uint8_t *packets, std::size_t packet_bytes, const CapturedDraw &draw,
                          bool world_channels) {
  ChannelScan scan;
  scan.items.resize(draw.item_count);
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    const CapturedItem &item = draw.items[index];
    ChannelScan::ItemInfo &info = scan.items[index];
    if (item.packet_bytes == 0) continue;
    if (item.packet_offset > packet_bytes || item.packet_bytes > packet_bytes - item.packet_offset) {
      scan.errors.push_back("item packet range outside the capture block");
      continue;
    }
    scan.packets += 1;
    const VifPacket packet = decode_packet(packets + item.packet_offset, item.packet_bytes);
    if (!packet.ok) {
      scan.errors.push_back(packet.error);
      continue;
    }
    scan.decoded += 1;
    info.decoded = true;
    read_descriptor(packet, info);
    read_position_family(packet, info);
    for (const VifCommand &entry : packet.commands) {
      if (!entry.unpack) continue;
      const ChannelRole role = classify(entry.tops, entry.destination, entry.format);
      if (role == ChannelRole::uv && (world_channels || entry.format != ChannelFormat::v2_8)) info.has_uv = true;
      if (role == ChannelRole::uv_aux && world_channels) info.has_uv = true;
      ShapeChannel channel;
      channel.item = index;
      channel.command = static_cast<std::uint32_t>(&entry - packet.commands.data());
      channel.tops = entry.tops;
      channel.destination = entry.destination;
      channel.format = entry.format;
      channel.vectors = entry.vectors;
      channel.role = role;
      scan.channels.push_back(channel);
    }
  }
  return scan;
}

bool build_mesh(const std::uint8_t *packets, std::size_t packet_bytes, const CapturedDraw &draw,
                const MeshOptions &options, Mesh &mesh, std::string &error) {
  mesh = Mesh{};

  std::vector<ItemData> items;
  items.reserve(draw.item_count);
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    const CapturedItem &item = draw.items[index];
    if (item.packet_bytes == 0) continue;
    if (item.packet_offset > packet_bytes || item.packet_bytes > packet_bytes - item.packet_offset) {
      error = "item packet range outside the capture block";
      return false;
    }
    const VifPacket packet = decode_packet(packets + item.packet_offset, item.packet_bytes);
    if (!packet.ok) {
      error = "VIF decode: " + packet.error;
      return false;
    }
    ItemData data;
    data.item_index = index;
    ChannelScan::ItemInfo info;
    read_descriptor(packet, info);
    read_position_family(packet, info);
    if (options.world_channels && info.has_descriptor) {
      // The descriptor-driven world path (TASK-090 track M1): one contiguous vertex list at qword 0, count from lane 0.
      const std::uint32_t dispatch = item.run_valid != 0 ? (item.run_itop & 0x3FFu) * 8u : 0u;
      if (!read_world_item(packet, info, dispatch, options, data, error)) {
        error = "item " + std::to_string(index) + ": " + error;
        return false;
      }
      items.push_back(std::move(data));
      continue;
    }
    // The original car-pass path, unchanged: per-channel role decoding with the channels' own vector counts.
    for (const VifCommand &entry : packet.commands) {
      if (!entry.unpack) continue;
      const ChannelRole role = classify(entry.tops, entry.destination, entry.format);
      const std::uint8_t *image = entry.tops ? packet.packed.data() : packet.window.data();
      std::vector<VifVector> vectors;
      if (!read_vu_vectors(image, entry.destination * 16u, entry.vectors, vectors)) {
        error = "channel outside the decoded VU image";
        return false;
      }
      switch (role) {
      case ChannelRole::positions:
        for (const VifVector &vector : vectors) {
          const bool floats = entry.format == ChannelFormat::v3_32;
          const float scale = floats ? 1.0f : options.position_scale;
          data.positions.push_back(floats ? decode_bits(vector.v[0]) : decode_signed16(vector.v[0]) * scale);
          data.positions.push_back(floats ? decode_bits(vector.v[1]) : decode_signed16(vector.v[1]) * scale);
          data.positions.push_back(floats ? decode_bits(vector.v[2]) : decode_signed16(vector.v[2]) * scale);
          if (!floats) {
            data.raw_positions.push_back(as_s16(vector.v[0]));
            data.raw_positions.push_back(as_s16(vector.v[1]));
            data.raw_positions.push_back(as_s16(vector.v[2]));
          } else {
            data.raw16 = false;
          }
        }
        break;
      case ChannelRole::normals:
        if (!options.decode_normals) break;
        data.has_normals = true;
        for (const VifVector &vector : vectors)
          for (std::uint32_t lane = 0; lane < 3; ++lane)
            data.normals.push_back(static_cast<std::int8_t>(vector.v[lane] & 0xFFu));
        break;
      case ChannelRole::normals80:
        data.has_normals80 = true;
        for (const VifVector &vector : vectors)
          for (std::uint32_t lane = 0; lane < 3; ++lane)
            data.normals80.push_back(static_cast<std::int8_t>(vector.v[lane] & 0xFFu));
        break;
      case ChannelRole::normals80_float:
        data.has_normals80_float = true;
        for (const VifVector &vector : vectors)
          for (std::uint32_t lane = 0; lane < 3; ++lane) data.normals80_float.push_back(decode_bits(vector.v[lane]));
        break;
      case ChannelRole::color:
        data.has_colors = true;
        for (const VifVector &vector : vectors)
          for (std::uint32_t lane = 0; lane < 4; ++lane)
            data.colors.push_back(static_cast<std::uint8_t>(vector.v[lane] & 0xFFu));
        break;
      case ChannelRole::uv:
        // The legacy (car) path never treated V2-8 at qword 0x40 as the model UV: the measured car frames and fixtures
        // predate that format and must stay byte-identical. The world path reads V2-8 through read_world_uv instead.
        if (entry.format == ChannelFormat::v2_8) break;
        data.has_uv = true;
        // Measured convention (coregs3_shape.hpp): a 16-bit channel is a signed fixed-point fraction divided by 4096,
        // with no V flip; a 32-bit channel is float UVs passed through unchanged. The first two components are the
        // UVs; a V3 channel's third component is skipped. A V2-8 channel is the 8-bit counterpart (signed / 16), a
        // Hypothesis used only by the world path's report; the car capture never selects one as its model UV.
        for (const VifVector &vector : vectors) {
          const bool floats = entry.format == ChannelFormat::v2_32 || entry.format == ChannelFormat::v3_32;
          data.uvs.push_back(floats ? decode_bits(vector.v[0]) : decode_signed16(vector.v[0]) / 4096.0f);
          data.uvs.push_back(floats ? decode_bits(vector.v[1]) : decode_signed16(vector.v[1]) / 4096.0f);
        }
        break;
      case ChannelRole::strip_table:
        // A later table in the same packet overwrites the earlier one on the VU (same destination), so keep the last.
        // The VU1 program writes the table through STMASK 0x3F3F3F3F, which lands the byte in lane 3 only (measured;
        // docs/COREGS3-HOOKS.md §6). An unmasked write broadcasts it to every lane, so lane 3 is the stable source.
        data.strip_table.clear();
        for (const VifVector &vector : vectors)
          data.strip_table.push_back(static_cast<std::uint8_t>(vector.v[3] & 0xFFu));
        break;
      default: break;
      }
    }
    if (data.vertices() != 0) items.push_back(std::move(data));
  }

  std::uint32_t vertex_count = 0;
  for (const ItemData &item : items) vertex_count += item.vertices();
  if (vertex_count == 0) {
    error = "no position channel";
    return false;
  }

  mesh.vertices.resize(vertex_count);
  std::uint32_t base = 0;
  for (const ItemData &item : items) {
    if (item.has_descriptor && mesh.has_descriptor == false) {
      mesh.has_descriptor = true;
      mesh.descriptor_vertices = item.descriptor_vertices;
      mesh.descriptor_selector = item.descriptor_selector;
      mesh.descriptor_signature = item.descriptor_signature;
    }
    for (std::uint32_t index = 0; index < item.vertices(); ++index) {
      MeshVertex &vertex = mesh.vertices[base + index];
      vertex.item = item.item_index;
      vertex.encoding = item.encoding;
      vertex.position = {item.positions[index * 3 + 0], item.positions[index * 3 + 1], item.positions[index * 3 + 2]};
      if (item.raw16 && (index + 1) * 3 <= item.raw_positions.size()) {
        vertex.raw_position = {item.raw_positions[index * 3 + 0], item.raw_positions[index * 3 + 1],
                               item.raw_positions[index * 3 + 2]};
        vertex.has_raw_position = true;
      }
      if (item.encoding == PositionEncoding::integer_32 && (index + 1) * 3 <= item.lane_positions.size()) {
        vertex.lane_position = {item.lane_positions[index * 3 + 0], item.lane_positions[index * 3 + 1],
                                item.lane_positions[index * 3 + 2]};
        vertex.has_lane_position = true;
      }
      if (item.has_normals && (index + 1) * 3 <= item.normals.size()) {
        vertex.normal = {item.normals[index * 3 + 0], item.normals[index * 3 + 1], item.normals[index * 3 + 2]};
        vertex.has_normal = true;
      }
      if (item.has_normals80 && (index + 1) * 3 <= item.normals80.size()) {
        vertex.normal80 = {item.normals80[index * 3 + 0], item.normals80[index * 3 + 1],
                           item.normals80[index * 3 + 2]};
        vertex.has_normal80 = true;
      }
      if (item.has_normals80_float && (index + 1) * 3 <= item.normals80_float.size()) {
        vertex.normal80_float = {item.normals80_float[index * 3 + 0], item.normals80_float[index * 3 + 1],
                                 item.normals80_float[index * 3 + 2]};
        vertex.has_normal80_float = true;
      }
      if (item.has_colors && (index + 1) * 4 <= item.colors.size()) {
        for (std::uint32_t lane = 0; lane < 4; ++lane) vertex.color[lane] = item.colors[index * 4 + lane];
        vertex.has_color = true;
      }
      if (item.has_uv && (index + 1) * 2 <= item.uvs.size()) {
        vertex.uv = {item.uvs[index * 2 + 0], item.uvs[index * 2 + 1]};
        vertex.has_uv = true;
      }
    }
    mesh.has_colors = mesh.has_colors || item.has_colors;
    mesh.has_normals = mesh.has_normals || item.has_normals;
    mesh.has_normals80 = mesh.has_normals80 || item.has_normals80;
    mesh.has_normals80_float = mesh.has_normals80_float || item.has_normals80_float;
    mesh.has_uv = mesh.has_uv || item.has_uv;
    if (!item.strip_table.empty()) mesh.has_strip_table = true;
    base += item.vertices();
  }

  if (options.topology == Topology::list) {
    for (std::uint32_t vertex = 0; vertex + 2 < vertex_count; vertex += 3) {
      mesh.strip_starts.push_back(vertex);
      mesh.strip_lengths.push_back(3);
      mesh.indices.push_back(vertex);
      mesh.indices.push_back(vertex + 1);
      mesh.indices.push_back(vertex + 2);
    }
    if (mesh.indices.empty()) {
      error = "no triangles (fewer than three vertices)";
      return false;
    }
    return true;
  }
  if (options.topology == Topology::quad) {
    // Diagnostic only: two triangles per four vertices, ignoring the strip table.
    for (std::uint32_t vertex = 0; vertex + 3 < vertex_count; vertex += 4) {
      mesh.strip_starts.push_back(vertex);
      mesh.strip_lengths.push_back(4);
      mesh.indices.push_back(vertex + 0);
      mesh.indices.push_back(vertex + 1);
      mesh.indices.push_back(vertex + 2);
      mesh.indices.push_back(vertex + 0);
      mesh.indices.push_back(vertex + 2);
      mesh.indices.push_back(vertex + 3);
    }
    if (mesh.indices.empty()) {
      error = "no triangles (fewer than four vertices)";
      return false;
    }
    return true;
  }

  // Strip topology: each item's strip table says how many vertices every strip has (a table with no bytes is one strip
  // over the whole item). A strip of n vertices has n-2 triangles; the winding alternates from the strip's first vertex.
  base = 0;
  for (std::uint32_t item_index = 0; item_index < items.size(); ++item_index) {
    const ItemData &item = items[item_index];
    std::vector<std::uint32_t> lengths;
    std::string table_error;
    if (!decode_strip_table(item.strip_table, item.vertices(), lengths, table_error)) {
      error = "item " + std::to_string(item_index) + ": " + table_error;
      return false;
    }
    std::uint32_t begin = base;
    for (const std::uint32_t length : lengths) {
      const std::uint32_t end = begin + length;
      mesh.strip_starts.push_back(begin);
      mesh.strip_lengths.push_back(length);
      for (std::uint32_t vertex = begin; vertex + 2 < end; ++vertex) {
        const bool flip = ((vertex - begin) & 1u) != 0;
        const std::uint32_t a = vertex + (flip ? 1u : 0u);
        const std::uint32_t b = vertex + (flip ? 0u : 1u);
        const std::uint32_t c = vertex + 2;
        if (options.max_edge > 0.0f) {
          auto far = [&](std::uint32_t first, std::uint32_t second) {
            const auto &pa = mesh.vertices[first].position;
            const auto &pb = mesh.vertices[second].position;
            const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
            return dx * dx + dy * dy + dz * dz > options.max_edge * options.max_edge;
          };
          if (far(a, b) || far(b, c) || far(a, c)) continue;
        }
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
      }
      begin = end;
    }
    base += item.vertices();
  }
  if (mesh.indices.empty()) {
    error = "no triangles (strip list is too short)";
    return false;
  }
  return true;
}

}  // namespace gt4::coregs3
