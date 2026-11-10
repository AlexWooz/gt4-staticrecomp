// TASK-090 CoreGS 3.0: the `.gt4scene` serialization. See coregs3_scene.hpp for the layout and the bounds.
//
// The writer and the reader are the same file on purpose: a fixture writes a snapshot and reads it back, so a change to
// one side that the other does not follow fails the round trip before a capture is ever trusted.
#include "coregs3_scene.hpp"

#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

// The spans must add up to the fixed word count the draw record reserves.
static_assert(kPglSpans[0].words + kPglSpans[1].words + kPglSpans[2].words + kPglSpans[3].words + kPglSpans[4].words ==
                  kPglWords,
              "kPglWords must be the sum of the captured PGL spans");
static_assert(kEnvSpans[0].words + kEnvSpans[1].words + kEnvSpans[2].words == kEnvWords,
              "kEnvWords must be the sum of the scene environment spans");

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

void put_pass_state(std::vector<std::uint8_t> &out, const CapturedPassState &state) {
  put_u32(out, state.pass);
  put_u32(out, state.first_draw);
  put_u32(out, state.draw_count);
  put_u32(out, state.item_count);
  put_u32(out, state.flags);
  put_u32(out, state.reserved);
  put_u64(out, state.vblank);
  put_u64(out, state.fogcol);
  put_u64(out, state.frame);
  put_u64(out, state.xyoffset);
  put_u64(out, state.zbuf);
  put_u64(out, state.scissor);
  put_u64(out, state.alpha);
  put_u64(out, state.test);
}

bool get_pass_state(Reader &in, CapturedPassState &state) {
  state.pass = in.u32();
  state.first_draw = in.u32();
  state.draw_count = in.u32();
  state.item_count = in.u32();
  state.flags = in.u32();
  state.reserved = in.u32();
  state.vblank = in.u64();
  state.fogcol = in.u64();
  state.frame = in.u64();
  state.xyoffset = in.u64();
  state.zbuf = in.u64();
  state.scissor = in.u64();
  state.alpha = in.u64();
  state.test = in.u64();
  if (state.pass >= kPassCount) return false;
  if (state.first_draw != kNoRecord && state.first_draw >= kMaxDrawsPerFrame) return false;
  return in.ok;
}

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
  put_u32(out, draw.light_block_qwords);
  put_u32(out, draw.light_block_command);
  put_u32(out, draw.light_block_source);
  for (const auto &qword : draw.light_block)
    for (std::uint32_t word : qword) put_u32(out, word);
  put_u32(out, draw.program_tag);
  put_u32(out, draw.program_selector);
  put_u32(out, draw.program_entry);
  put_u32(out, draw.program_itop);
  put_u32(out, draw.program_dispatch);
  put_u32(out, draw.program_count);
  put_u32(out, draw.program_valid);
  put_u32(out, draw.chain_tags);
  put_u32(out, draw.chain_valid);
  put_u32(out, draw.program_uniform);
  put_u32(out, draw.call_site);
  put_u32(out, draw.caller_ra);
  // Format version 6: the 0x00A0 morph factor and its evidence.
  put_u32(out, draw.morph_factor);
  put_u32(out, draw.morph_factor_pgl);
  put_u32(out, draw.morph_factor_valid);
  put_u32(out, draw.morph_uploads);
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
    // Format version 4: the per-item VU1 light state and the run pair.
    put_u32(out, item.light_block_qwords);
    put_u32(out, item.light_block_command);
    put_u32(out, item.light_block_source);
    put_u32(out, item.light_source);
    put_u32(out, item.run_itop);
    put_u32(out, item.run_mscal);
    put_u32(out, item.run_valid);
    for (const auto &qword : item.light_block)
      for (std::uint32_t word : qword) put_u32(out, word);
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
  draw.light_block_qwords = in.u32();
  if (draw.light_block_qwords > kMaxLightBlockQwords) return false;
  draw.light_block_command = in.u32();
  draw.light_block_source = in.u32();
  for (auto &qword : draw.light_block)
    for (std::uint32_t &word : qword) word = in.u32();
  draw.program_tag = in.u32();
  draw.program_selector = in.u32();
  draw.program_entry = in.u32();
  draw.program_itop = in.u32();
  draw.program_dispatch = in.u32();
  draw.program_count = in.u32();
  draw.program_valid = in.u32();
  if (draw.program_valid > 1) return false;
  draw.chain_tags = in.u32();
  draw.chain_valid = in.u32();
  draw.program_uniform = in.u32();
  if (draw.chain_valid > 1 || draw.program_uniform > 1) return false;
  draw.call_site = in.u32();
  draw.caller_ra = in.u32();
  if (draw.call_site > kCallSiteTachometer) return false;
  // Format version 6: the morph factor and its evidence.
  draw.morph_factor = in.u32();
  draw.morph_factor_pgl = in.u32();
  draw.morph_factor_valid = in.u32();
  draw.morph_uploads = in.u32();
  if (draw.morph_factor_valid > (kMorphUploadSeen | kMorphPglRead | kMorphValuesAgree)) return false;
  if (draw.morph_uploads > kMaxItemsPerDraw) return false;
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    CapturedItem &item = draw.items[index];
    item.address = in.u32();
    item.qwords = in.u16();
    item.event = in.u16();
    item.texture_record = in.u32();
    item.material_record = in.u32();
    item.packet_offset = in.u32();
    item.packet_bytes = in.u32();
    item.light_block_qwords = in.u32();
    if (item.light_block_qwords > kMaxLightBlockQwords) return false;
    item.light_block_command = in.u32();
    item.light_block_source = in.u32();
    item.light_source = in.u32();
    if (item.light_source > kLightItemCarriedUncertain) return false;
    item.run_itop = in.u32();
    item.run_mscal = in.u32();
    item.run_valid = in.u32();
    if (item.run_valid > 1) return false;
    for (auto &qword : item.light_block)
      for (std::uint32_t &word : qword) word = in.u32();
  }
  return in.ok;
}

}  // namespace

Mode parse_mode(const char *text) noexcept {
  if (text == nullptr || *text == '\0') return Mode::off;
  if (std::strcmp(text, "off") == 0) return Mode::off;
  if (std::strcmp(text, "shadow") == 0) return Mode::shadow;
  if (std::strcmp(text, "overlay") == 0) return Mode::overlay;
  if (std::strcmp(text, "present") == 0) return Mode::present;
  return Mode::off;
}

const char *mode_name(Mode mode) noexcept {
  switch (mode) {
  case Mode::off: return "off";
  case Mode::shadow: return "shadow";
  case Mode::overlay: return "overlay";
  case Mode::present: return "present";
  }
  return "off";
}

const char *pass_name(std::uint32_t pass) noexcept {
  switch (pass) {
  case kPassUnknown: return "unknown";
  case kPassEnvMap: return "env_map";
  case kPassSky: return "sky";
  case kPassFarUnderlay: return "far_underlay";
  case kPassBackdrop: return "backdrop";
  case kPassCourse: return "course";
  case kPassBillboards: return "billboards";
  case kPassReflection: return "reflection";
  case kPassCars: return "cars";
  case kPassRenderAfter: return "render_after";
  case kPassMirror: return "mirror";
  case kPassFlares: return "flares";
  case kPassCrowdPitmen: return "crowd_pitmen";
  case kPassParticles: return "particles";
  case kPassPost: return "post";
  case kPassPanel: return "panel";
  case kPassConcourse: return "concourse";
  default: return "unknown";
  }
}

const char *call_site_name(std::uint32_t site) noexcept {
  switch (site) {
  case kCallSiteModelSet2: return "model_set2";
  case kCallSiteBillboard: return "billboard";
  case kCallSiteEnvMap: return "env_map";
  case kCallSiteTachometer: return "tachometer";
  default: return "unknown";
  }
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
    // Version 1 predates the per-draw light block, version 2 the per-draw VU1 program selector, version 3 the per-item
    // light state and version 4 the pass tag, the environment block and the pass journal, so their records are shorter
    // (see the format note in the header). There is no migration path: the capture is a diagnostic artifact and is
    // recaptured with a matching build.
    error = "unsupported .gt4scene version " + std::to_string(header.version) + " (this build writes and reads version " +
            std::to_string(kSceneVersion) +
            "; version 1 has no light block, version 2 no program selector, version 3 no per-item light state, version 4 "
            "no pass tag/environment/journal, version 5 no 0x00A0 morph factor; all older versions are refused)";
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
  if (frame.environment.valid > 1) return false;
  if (frame.pass_states.size() > kMaxPassStatesPerFrame) return false;
  for (const CapturedPassState &state : frame.pass_states) {
    if (state.pass >= kPassCount) return false;
    if (state.first_draw != kNoRecord && state.first_draw >= kMaxDrawsPerFrame) return false;
    if (state.first_draw == kNoRecord && state.draw_count != 0) return false;
  }
  for (const CapturedDraw &draw : frame.draws) {
    if (draw.pass >= kPassCount) return false;
    if (draw.call_site > kCallSiteTachometer) return false;
    if (draw.item_count > kMaxItemsPerDraw) return false;
    if (draw.light_block_qwords > kMaxLightBlockQwords) return false;
    if (draw.program_valid > 1 || draw.chain_valid > 1 || draw.program_uniform > 1) return false;
    // Format version 6: the morph flags are a bit mask and the upload count cannot exceed the item table.
    if (draw.morph_factor_valid > (kMorphUploadSeen | kMorphPglRead | kMorphValuesAgree)) return false;
    if (draw.morph_uploads > kMaxItemsPerDraw) return false;
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      if (item.texture_record != kNoRecord && item.texture_record >= frame.texture_count) return false;
      if (item.material_record != kNoRecord && item.material_record >= frame.material_count) return false;
      if (item.light_block_qwords > kMaxLightBlockQwords) return false;
      if (item.light_source > kLightItemCarriedUncertain) return false;
      if (item.run_valid > 1) return false;
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
  // Format version 5: the scene environment block and the per-pass GS state journal.
  put_u32(body, frame.environment.valid);
  put_u32(body, 0u);
  for (std::uint32_t word : frame.environment.words) put_u32(body, word);
  put_u32(body, static_cast<std::uint32_t>(frame.pass_states.size()));
  put_u32(body, 0u);
  for (const CapturedPassState &state : frame.pass_states) put_pass_state(body, state);
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
  // Format version 5: the scene environment block and the pass journal.
  frame.environment.valid = content.u32();
  (void)content.u32();
  if (frame.environment.valid > 1) {
    error = "environment validity flag out of range";
    return 0;
  }
  for (std::uint32_t &word : frame.environment.words) word = content.u32();
  const std::uint32_t pass_state_count = content.u32();
  (void)content.u32();
  if (!content.ok) {
    error = "truncated environment record";
    return 0;
  }
  if (pass_state_count > kMaxPassStatesPerFrame) {
    error = "pass journal exceeds the format bound";
    return 0;
  }
  frame.pass_states.resize(pass_state_count);
  for (CapturedPassState &state : frame.pass_states)
    if (!get_pass_state(content, state)) break;
  if (!content.ok) {
    error = "truncated pass journal";
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
  bool draws_ok = true;
  for (CapturedDraw &draw : frame.draws)
    if (!get_draw(content, version, draw)) {
      draws_ok = false;
      break;
    }
  if (!content.ok) {
    error = "truncated frame records";
    return 0;
  }
  if (!draws_ok) {
    // get_draw refuses a field outside the format's bounds (a record index, an enum, a block length, a flag mask) even
    // when every byte was present; the block is refused rather than read with the bad value.
    error = "draw record outside the format bounds";
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

MorphBlend morph_blend_of(const CapturedDraw &draw) noexcept {
  MorphBlend blend;
  if ((draw.morph_factor_valid & kMorphUploadSeen) == 0) return blend;
  float factor = 0.0f;
  std::memcpy(&factor, &draw.morph_factor, sizeof factor);
  if (!std::isfinite(factor)) return blend;
  blend.valid = true;
  blend.factor = factor;
  blend.bits = draw.morph_factor;
  return blend;
}

}  // namespace gt4::coregs3
