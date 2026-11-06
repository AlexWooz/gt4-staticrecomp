// TASK-090 CoreGS 3.0: the shape channel scan and the default role rule. See coregs3_shape.hpp.
#include "coregs3_shape.hpp"

#include "coregs3_scene.hpp"

#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

ChannelRole classify(bool tops, std::uint32_t destination, ChannelFormat format) {
  if (tops && destination == 0x00u) {
    if (format == ChannelFormat::v3_16 || format == ChannelFormat::v3_32) return ChannelRole::positions;
    return ChannelRole::unknown;
  }
  // The strip table sits at TOPS-relative qword 0x40 like the UVs, distinguished by its single 8-bit component. The
  // VU1 program writes it through STMASK 0x3F3F3F3F, so only lane 3 of each qword changes; see decode_strip_table.
  if (tops && destination == 0x40u && format == ChannelFormat::v1_8) return ChannelRole::strip_table;
  if (tops && destination == 0x40u) {
    switch (format) {
    case ChannelFormat::v2_16:
    case ChannelFormat::v2_32:
    case ChannelFormat::v3_16:
    case ChannelFormat::v3_32: return ChannelRole::uv;
    default: return ChannelRole::unknown;
    }
  }
  if (tops && destination == 0x80u && format == ChannelFormat::v4_8) return ChannelRole::color;
  // An absolute V3-32 vector at qword 0 is a float position channel (the wheel and tyre shapes use it).
  if (!tops && destination == 0x00u && format == ChannelFormat::v3_32) return ChannelRole::positions;
  return ChannelRole::auxiliary;
}

// VIF 16-bit UNPACK stores the raw halfword; the model scale is applied by the caller's position_scale.
float decode_signed16(std::uint32_t raw) { return static_cast<float>(static_cast<std::int16_t>(raw & 0xFFFFu)); }
float decode_bits(std::uint32_t raw) {
  float value = 0.0f;
  std::memcpy(&value, &raw, sizeof value);
  return value;
}

struct ItemData {
  std::vector<float> positions;      // 3 floats per vertex
  std::vector<std::uint8_t> colors;  // 4 bytes per vertex
  std::vector<float> uvs;            // 2 floats per vertex
  std::vector<std::uint8_t> strip_table;
  bool has_colors = false;
  bool has_uv = false;
  std::uint32_t vertices() const { return static_cast<std::uint32_t>(positions.size() / 3); }
};

}  // namespace

const char *channel_role_name(ChannelRole role) noexcept {
  switch (role) {
  case ChannelRole::positions: return "positions";
  case ChannelRole::uv: return "uv";
  case ChannelRole::color: return "color";
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

ChannelScan scan_channels(const std::uint8_t *packets, std::size_t packet_bytes, const CapturedDraw &draw) {
  ChannelScan scan;
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    const CapturedItem &item = draw.items[index];
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
    for (std::uint32_t command = 0; command < packet.commands.size(); ++command) {
      const VifCommand &entry = packet.commands[command];
      if (!entry.unpack) continue;
      ShapeChannel channel;
      channel.item = index;
      channel.command = command;
      channel.tops = entry.tops;
      channel.destination = entry.destination;
      channel.format = entry.format;
      channel.vectors = entry.vectors;
      channel.role = classify(entry.tops, entry.destination, entry.format);
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
        }
        break;
      case ChannelRole::color:
        data.has_colors = true;
        for (const VifVector &vector : vectors)
          for (std::uint32_t lane = 0; lane < 4; ++lane)
            data.colors.push_back(static_cast<std::uint8_t>(vector.v[lane] & 0xFFu));
        break;
      case ChannelRole::uv:
        data.has_uv = true;
        for (const VifVector &vector : vectors) {
          data.uvs.push_back(static_cast<float>(static_cast<std::uint16_t>(vector.v[0] & 0xFFFFu)) / 65536.0f);
          const float v = static_cast<float>(static_cast<std::uint16_t>(vector.v[1] & 0xFFFFu)) / 65536.0f;
          data.uvs.push_back(options.flip_v ? 1.0f - v : v);
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
    for (std::uint32_t index = 0; index < item.vertices(); ++index) {
      MeshVertex &vertex = mesh.vertices[base + index];
      vertex.position = {item.positions[index * 3 + 0], item.positions[index * 3 + 1], item.positions[index * 3 + 2]};
      if (item.has_colors && (index + 1) * 4 <= item.colors.size())
        for (std::uint32_t lane = 0; lane < 4; ++lane) vertex.color[lane] = item.colors[index * 4 + lane];
      if (item.has_uv && (index + 1) * 2 <= item.uvs.size()) {
        vertex.uv = {item.uvs[index * 2 + 0], item.uvs[index * 2 + 1]};
        vertex.has_uv = true;
      }
    }
    mesh.has_colors = mesh.has_colors || item.has_colors;
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
