// TASK-090 CoreGS 3.0: the `.gt4scene` serialization. See coregs3_scene.hpp for the layout and the bounds.
//
// The writer and the reader are the same file on purpose: a fixture writes a snapshot and reads it back, so a change to
// one side that the other does not follow fails the round trip before a capture is ever trusted.
#include "coregs3_scene.hpp"

#include <cstring>

namespace gt4::coregs3 {
namespace {

// The spans must add up to the fixed word count the draw record reserves.
static_assert(kPglSpans[0].words + kPglSpans[1].words + kPglSpans[2].words + kPglSpans[3].words + kPglSpans[4].words ==
                  kPglWords,
              "kPglWords must be the sum of the captured PGL spans");

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

void put_bytes(std::vector<std::uint8_t> &out, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  out.insert(out.end(), bytes, bytes + size);
}

struct Reader {
  const std::uint8_t *data;
  std::size_t size;
  std::size_t at = 0;
  bool ok = true;

  bool need(std::size_t bytes) {
    if (!ok || bytes > size - at) {
      ok = false;
      return false;
    }
    return true;
  }
  std::uint16_t u16() {
    if (!need(2)) return 0;
    const std::uint16_t value = static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
    at += 2;
    return value;
  }
  std::uint32_t u32() {
    if (!need(4)) return 0;
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(data[at++]) << shift;
    return value;
  }
  std::uint64_t u64() {
    if (!need(8)) return 0;
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(data[at++]) << shift;
    return value;
  }
  bool bytes(void *destination, std::size_t count) {
    if (!need(count)) return false;
    std::memcpy(destination, data + at, count);
    at += count;
    return true;
  }
  void skip(std::size_t count) {
    if (need(count)) at += count;
  }
};

void put_draw(std::vector<std::uint8_t> &out, const CapturedDraw &draw) {
  put_u32(out, draw.sequence);
  put_u32(out, draw.pass);
  put_u32(out, draw.shape);
  put_u32(out, draw.cmd_a);
  put_u32(out, draw.cmd_value);
  put_u32(out, draw.cmd_flag);
  put_u32(out, draw.cmd_bits);
  put_u32(out, draw.cmd_count);
  put_u32(out, draw.cmd_nc);
  put_u32(out, draw.cmd_nd);
  put_u32(out, draw.item_count);
  for (std::uint32_t word : draw.pgl) put_u32(out, word);
  for (const auto &row : draw.vf)
    for (std::uint32_t word : row) put_u32(out, word);
  put_u32(out, draw.matrix_packet_valid);
  for (const auto &row : draw.matrix_packet)
    for (std::uint32_t word : row) put_u32(out, word);
  // Only the items the draw declares: an empty item slot costs nothing on disk.
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    const CapturedItem &item = draw.items[index];
    put_u32(out, item.address);
    put_u16(out, item.qwords);
    put_u16(out, item.event);
    put_u32(out, item.texture_record);
    put_u32(out, item.material_record);
    put_u32(out, item.packet_offset);
    put_u32(out, item.packet_bytes);
  }
}

bool get_draw(Reader &in, std::uint32_t version, CapturedDraw &draw) {
  (void)version;
  draw.sequence = in.u32();
  draw.pass = in.u32();
  draw.shape = in.u32();
  draw.cmd_a = in.u32();
  draw.cmd_value = in.u32();
  draw.cmd_flag = in.u32();
  draw.cmd_bits = in.u32();
  draw.cmd_count = in.u32();
  draw.cmd_nc = in.u32();
  draw.cmd_nd = in.u32();
  draw.item_count = in.u32();
  if (draw.item_count > kMaxItemsPerDraw) return false;
  for (std::uint32_t &word : draw.pgl) word = in.u32();
  for (auto &row : draw.vf)
    for (std::uint32_t &word : row) word = in.u32();
  draw.matrix_packet_valid = in.u32();
  for (auto &row : draw.matrix_packet)
    for (std::uint32_t &word : row) word = in.u32();
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    CapturedItem &item = draw.items[index];
    item.address = in.u32();
    item.qwords = in.u16();
    item.event = in.u16();
    item.texture_record = in.u32();
    item.material_record = in.u32();
    item.packet_offset = in.u32();
    item.packet_bytes = in.u32();
  }
  return in.ok;
}

}  // namespace

Mode parse_mode(const char *text) noexcept {
  if (text == nullptr || *text == '\0') return Mode::off;
  if (std::strcmp(text, "off") == 0) return Mode::off;
  if (std::strcmp(text, "shadow") == 0) return Mode::shadow;
  if (std::strcmp(text, "overlay") == 0) return Mode::overlay;
  return Mode::off;
}

const char *mode_name(Mode mode) noexcept {
  switch (mode) {
  case Mode::off: return "off";
  case Mode::shadow: return "shadow";
  case Mode::overlay: return "overlay";
  }
  return "off";
}

bool write_scene_header(std::vector<std::uint8_t> &output, const SceneHeader &header) {
  put_u32(output, kSceneMagic);
  put_u32(output, header.version);
  put_u32(output, header.target);
  put_u32(output, 0u);
  return true;
}

bool parse_scene_header(const std::uint8_t *data, std::size_t available, SceneHeader &header, std::string &error) {
  Reader in{data, available};
  const std::uint32_t magic = in.u32();
  header.version = in.u32();
  header.target = in.u32();
  in.u32();
  if (!in.ok || magic != kSceneMagic) {
    error = "not a .gt4scene capture (bad magic)";
    return false;
  }
  if (header.version != kSceneVersion) {
    error = "unsupported .gt4scene version " + std::to_string(header.version);
    return false;
  }
  if (header.target != kCaptureTargetV200) {
    error = "capture target " + std::to_string(header.target) + " is not the v2.00 target this reader knows";
    return false;
  }
  return true;
}

bool frame_block_bytes(const FrameSnapshot &frame, std::vector<std::uint8_t> &output) {
  if (frame.draw_count != frame.draws.size() || frame.texture_count != frame.textures.size() ||
      frame.material_count != frame.materials.size())
    return false;
  if (frame.draw_count > kMaxDrawsPerFrame || frame.texture_count > kMaxTextureRecords ||
      frame.material_count > kMaxMaterialRecords)
    return false;
  if (frame.packets.size() > kMaxPacketBytesPerFrame) return false;
  for (const CapturedDraw &draw : frame.draws) {
    if (draw.item_count > kMaxItemsPerDraw) return false;
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      if (item.texture_record != kNoRecord && item.texture_record >= frame.texture_count) return false;
      if (item.material_record != kNoRecord && item.material_record >= frame.material_count) return false;
      if (item.packet_bytes == 0) continue;
      if (item.packet_bytes > kMaxPacketBytesPerDraw) return false;
      if (item.packet_offset > frame.packets.size() || item.packet_bytes > frame.packets.size() - item.packet_offset)
        return false;
    }
  }

  std::vector<std::uint8_t> body;
  body.reserve(64 + frame.textures.size() * (4 + kTextureRecordBytes) +
               frame.materials.size() * (4 + kMaterialRecordBytes) + frame.draws.size() * 1024 +
               frame.packets.size());
  put_u32(body, kFrameMagic);
  put_u32(body, frame.index);
  put_u32(body, 0u);
  put_u64(body, frame.vblank);
  put_u32(body, frame.draw_count);
  put_u32(body, frame.texture_count);
  put_u32(body, frame.material_count);
  put_u32(body, static_cast<std::uint32_t>(frame.packets.size()));
  put_u32(body, frame.camera.valid);
  put_u32(body, frame.camera.camera);
  for (std::uint32_t word : frame.camera.pgl_block_k3) put_u32(body, word);
  for (std::uint32_t word : frame.camera.pgl_block_k4) put_u32(body, word);
  for (const auto &row : frame.camera.vf_k4)
    for (std::uint32_t word : row) put_u32(body, word);
  for (const CapturedTexture &texture : frame.textures) {
    put_u32(body, texture.address);
    put_bytes(body, texture.bytes, kTextureRecordBytes);
  }
  for (const CapturedMaterial &material : frame.materials) {
    put_u32(body, material.address);
    put_bytes(body, material.bytes, kMaterialRecordBytes);
  }
  for (const CapturedDraw &draw : frame.draws) put_draw(body, draw);
  put_bytes(body, frame.packets.data(), frame.packets.size());

  output.clear();
  put_u32(output, static_cast<std::uint32_t>(body.size() + 2 * sizeof(std::uint32_t)));
  put_u32(output, static_cast<std::uint32_t>(frame.overflow_flags & 0xFFFFFFFFu));
  put_bytes(output, body.data(), body.size());
  return true;
}

std::size_t parse_frame_block(const std::uint8_t *data, std::size_t available, std::uint32_t version,
                              FrameSnapshot &frame, std::string &error) {
  if (available < 8) {
    error = "truncated frame block";
    return 0;
  }
  Reader in{data, available};
  const std::uint32_t block_bytes = in.u32();
  const std::uint32_t overflow_low = in.u32();
  if (block_bytes < 8 || block_bytes > available) {
    error = "frame block length is out of range";
    return 0;
  }
  const std::uint8_t *body = data + 8;
  const std::size_t body_bytes = block_bytes - 8;
  Reader content{body, body_bytes};
  if (content.u32() != kFrameMagic) {
    error = "frame block magic mismatch";
    return 0;
  }
  frame = FrameSnapshot{};
  frame.overflow_flags = overflow_low;
  frame.index = content.u32();
  (void)content.u32();
  frame.vblank = content.u64();
  frame.draw_count = content.u32();
  frame.texture_count = content.u32();
  frame.material_count = content.u32();
  const std::uint32_t packet_bytes = content.u32();
  if (!content.ok) {
    error = "truncated frame header";
    return 0;
  }
  if (frame.draw_count > kMaxDrawsPerFrame || frame.texture_count > kMaxTextureRecords ||
      frame.material_count > kMaxMaterialRecords || packet_bytes > kMaxPacketBytesPerFrame) {
    error = "frame counts exceed the format bounds";
    return 0;
  }
  frame.camera.valid = content.u32();
  frame.camera.camera = content.u32();
  for (std::uint32_t &word : frame.camera.pgl_block_k3) word = content.u32();
  for (std::uint32_t &word : frame.camera.pgl_block_k4) word = content.u32();
  for (auto &row : frame.camera.vf_k4)
    for (std::uint32_t &word : row) word = content.u32();
  if (!content.ok) {
    error = "truncated camera record";
    return 0;
  }
  frame.textures.resize(frame.texture_count);
  for (CapturedTexture &texture : frame.textures) {
    texture.address = content.u32();
    if (!content.bytes(texture.bytes, kTextureRecordBytes)) break;
  }
  frame.materials.resize(frame.material_count);
  for (CapturedMaterial &material : frame.materials) {
    material.address = content.u32();
    if (!content.bytes(material.bytes, kMaterialRecordBytes)) break;
  }
  frame.draws.resize(frame.draw_count);
  for (CapturedDraw &draw : frame.draws)
    if (!get_draw(content, version, draw)) break;
  if (!content.ok) {
    error = "truncated frame records";
    return 0;
  }
  frame.packets.resize(packet_bytes);
  if (!content.bytes(frame.packets.data(), packet_bytes)) {
    error = "truncated packet block";
    return 0;
  }
  for (const CapturedDraw &draw : frame.draws) {
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      if (item.packet_bytes == 0) continue;
      if (item.packet_offset > packet_bytes || item.packet_bytes > packet_bytes - item.packet_offset ||
          item.packet_bytes > kMaxPacketBytesPerDraw) {
        error = "item packet range outside the packet block";
        return 0;
      }
    }
  }
  return block_bytes;
}

}  // namespace gt4::coregs3
