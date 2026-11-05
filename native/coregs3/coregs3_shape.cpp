// TASK-090 CoreGS 3.0: the shape channel scan and the default role rule. See coregs3_shape.hpp.
#include "coregs3_shape.hpp"

#include "coregs3_scene.hpp"

#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

ChannelRole classify(bool tops, std::uint32_t destination, ChannelFormat format) {
  // The reset list sits at qword 0x40 like the UVs, distinguished by its single 8-bit component: the car packets carry no
  // UV channel at all and use exactly this form (evidence: the channel scan of a captured night-race car draw).
  if (destination == 0x40u && format == ChannelFormat::v1_8) return ChannelRole::reset;
  if (tops) {
    if (destination == 0x00u) {
      if (format == ChannelFormat::v3_16 || format == ChannelFormat::v3_32) return ChannelRole::positions;
      return ChannelRole::unknown;
    }
    if (destination == 0x40u) {
      switch (format) {
      case ChannelFormat::v2_16:
      case ChannelFormat::v2_32:
      case ChannelFormat::v3_16:
      case ChannelFormat::v3_32: return ChannelRole::uv;
      default: return ChannelRole::unknown;
      }
    }
    if (destination == 0x80u && format == ChannelFormat::v4_8) return ChannelRole::color;
    return ChannelRole::auxiliary;
  }
  if (destination == 0x00u && format == ChannelFormat::v3_32) return ChannelRole::positions;
  return ChannelRole::auxiliary;
}

// VIF 16-bit UNPACK stores the raw halfword; the model scale is applied by the caller's position_scale.
float decode_signed16(std::uint32_t raw) { return static_cast<float>(static_cast<std::int16_t>(raw & 0xFFFFu)); }
float decode_bits(std::uint32_t raw) {
  float value = 0.0f;
  std::memcpy(&value, &raw, sizeof value);
  return value;
}

}  // namespace

const char *channel_role_name(ChannelRole role) noexcept {
  switch (role) {
  case ChannelRole::positions: return "positions";
  case ChannelRole::uv: return "uv";
  case ChannelRole::color: return "color";
  case ChannelRole::reset: return "reset";
  case ChannelRole::auxiliary: return "auxiliary";
  default: return "unknown";
  }
}

bool classify_reset(bool tops, std::uint32_t destination, ChannelFormat format) noexcept {
  return classify(tops, destination, format) == ChannelRole::reset;
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

  std::vector<float> positions;  // 3 per vertex
  std::vector<std::uint8_t> colors;
  std::vector<float> uvs;
  std::vector<std::uint8_t> resets;

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
          const float scale = entry.format == ChannelFormat::v3_32 ? 1.0f : options.position_scale;
          positions.push_back(entry.format == ChannelFormat::v3_32 ? decode_bits(vector.v[0])
                                                                  : decode_signed16(vector.v[0]) * scale);
          positions.push_back(entry.format == ChannelFormat::v3_32 ? decode_bits(vector.v[1])
                                                                  : decode_signed16(vector.v[1]) * scale);
          positions.push_back(entry.format == ChannelFormat::v3_32 ? decode_bits(vector.v[2])
                                                                  : decode_signed16(vector.v[2]) * scale);
        }
        break;
      case ChannelRole::color:
        mesh.has_colors = true;
        for (const VifVector &vector : vectors) {
          for (std::uint32_t lane = 0; lane < 4; ++lane)
            colors.push_back(static_cast<std::uint8_t>(vector.v[lane] & 0xFFu));
        }
        break;
      case ChannelRole::uv:
        mesh.has_uv = true;
        for (const VifVector &vector : vectors) {
          uvs.push_back(static_cast<float>(static_cast<std::uint16_t>(vector.v[0] & 0xFFFFu)) / 65536.0f);
          const float v = static_cast<float>(static_cast<std::uint16_t>(vector.v[1] & 0xFFFFu)) / 65536.0f;
          uvs.push_back(options.flip_v ? 1.0f - v : v);
        }
        break;
      case ChannelRole::reset:
        for (const VifVector &vector : vectors) resets.push_back(static_cast<std::uint8_t>(vector.v[0] & 0xFFu));
        break;
      default: break;
      }
    }
  }

  const std::size_t vertex_count = positions.size() / 3;
  if (vertex_count == 0) {
    error = "no position channel";
    return false;
  }
  mesh.vertices.resize(vertex_count);
  for (std::size_t index = 0; index < vertex_count; ++index) {
    MeshVertex &vertex = mesh.vertices[index];
    vertex.position = {positions[index * 3 + 0], positions[index * 3 + 1], positions[index * 3 + 2]};
    if (mesh.has_colors && (index + 1) * 4 <= colors.size()) {
      for (std::uint32_t lane = 0; lane < 4; ++lane) vertex.color[lane] = colors[index * 4 + lane];
    }
    if (mesh.has_uv && (index + 1) * 2 <= uvs.size()) {
      vertex.uv = {uvs[index * 2 + 0], uvs[index * 2 + 1]};
      vertex.has_uv = true;
    }
  }

  // Strip topology: the reset bytes give the first vertex of a new strip (0 is the start of the first one). Every other
  // strip continues from the previous vertex list, which is the layout of a GT4 shape: one vertex array per packet and
  // reset markers inside it.
  mesh.strip_starts.clear();
  mesh.strip_starts.push_back(0);
  for (std::uint8_t reset : resets) {
    const std::uint32_t start = reset;
    if (start == 0 || start >= vertex_count) continue;
    if (mesh.strip_starts.back() != start) mesh.strip_starts.push_back(start);
  }
  if (options.topology == Topology::list) {
    for (std::uint32_t vertex = 0; vertex + 2 < vertex_count; vertex += 3) {
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
  for (std::size_t index = 0; index < mesh.strip_starts.size(); ++index) {
    const std::uint32_t begin = mesh.strip_starts[index];
    const std::uint32_t end = index + 1 < mesh.strip_starts.size()
                                  ? mesh.strip_starts[index + 1]
                                  : static_cast<std::uint32_t>(vertex_count);
    if (end <= begin + 2) continue;
    for (std::uint32_t vertex = begin; vertex + 2 < end; ++vertex) {
      const bool flip = ((vertex - begin) & 1u) != 0;
      mesh.indices.push_back(vertex + (flip ? 1u : 0u));
      mesh.indices.push_back(vertex + (flip ? 0u : 1u));
      mesh.indices.push_back(vertex + 2);
    }
  }
  if (mesh.indices.empty()) {
    error = "no triangles (strip list is too short)";
    return false;
  }
  return true;
}

}  // namespace gt4::coregs3
