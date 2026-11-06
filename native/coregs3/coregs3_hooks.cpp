// TASK-090 CoreGS 3.0, phase 1: the observe-only EE hooks. See coregs3_hooks.hpp for the contract.
//
// Why these sites (docs/COREGS3-PLAN.md §3.1, §5.2, Confirmed against the decomp and the translated units):
//   * K1 0x00455A5C is the `jal 0x0049D308` of ModelSet2___render. a0 is the shape's Cmd; func_0049D308 passes the PGL
//     context object 0x70002000 as the second argument, so the whole draw description is guest state at this instant:
//     the Cmd and its item table (DMA REFs to the model's own VIF packets), the PGL shadow in scratchpad, the material
//     block func_0049E228 wrote at +0xA60 and the matrix block func_0049E548 uploaded from VU0 vf24..vf27 at +0xFA0.
//   * K3/K4 L_0033EBB4/L_0033EBF4 are the return labels of the projection and modelview virtual calls of func_0033EAB8.
//   * K5 L_0033EE28/L_0033EE54 bracket the single virtual call that renders the cars of the main view (vt+0x380, i.e.
//     0x00340950) inside func_0033EAB8. Only draws inside the bracket are stored (category `cars`).
//
// A draw is captured at K1's pre-call point: the item table and the PGL context are read before func_0049D328 consumes
// them, which is the only instant where both are still intact and no packet has been consumed by DMA. The post-call hook
// only counts completions (it is the fidelity control for "every started call returned", G-ROBUST).
#include "coregs3_hooks.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "coregs3_tap.hpp"
#include "mtvu_runtime.hpp"

#include "ps2/memory.hpp"
#include "ps2/system.hpp"

namespace gt4::coregs3 {
namespace {

constexpr std::uint32_t kRamLimit = 0x02000000u;
constexpr std::uint32_t kScratchBase = 0x70000000u;
constexpr std::uint32_t kScratchLimit = kScratchBase + 0x4000u;

struct State {
  Mode mode = Mode::off;
  bool configured = false;
  bool refuse_overlay_reported = false;
  std::FILE *capture = nullptr;
  std::uint64_t capture_first = 0;
  std::uint64_t capture_last = 0;
  bool capture_range = false;
  std::uint64_t capture_max_frames = 32;
  std::uint64_t trace_frames = 0;
  std::uint64_t trace_events = 0;
  std::uint64_t vblank = 0;
  bool in_car_pass = false;
  bool frame_open = false;
  bool all_draws = false;
  std::uint64_t pass_count = 0;
  std::uint64_t pass_job_first = 0, pass_job_last = 0;
  std::uint64_t orphan_pass_ends = 0;
  std::uint32_t frame_index = 0;
  std::uint64_t frame_vblank = 0;
  std::uint32_t sequence = 0;
  bool pending = false;
  std::uint32_t pending_sequence = 0;
  std::uint32_t pending_out = 0;   // PGL output cursor (context +0x1F8) at the pre-call point
  std::uint32_t pending_draw = kNoRecord;
  bool frame_overflow = false;
  std::uint64_t frame_overflow_flags = 0;
  FrameSnapshot frame;
  std::vector<std::uint8_t> block;
  std::vector<std::vector<std::uint8_t>> pending_blocks;  // frame blocks waiting for the shutdown flush
  CaptureCounters counters;
  CameraSnapshot camera;
};

State g;

bool read_guest(std::uint32_t address, void *destination, std::size_t bytes) noexcept {
  const auto *source = static_cast<const std::uint8_t *>(nullptr);
  if (address < kRamLimit && bytes <= kRamLimit - address) {
    source = ps2::g_ram + address;
  } else if (address >= kScratchBase && address < kScratchLimit && bytes <= kScratchLimit - address) {
    source = ps2::g_spr + (address - kScratchBase);
  } else {
    return false;
  }
  std::memcpy(destination, source, bytes);
  return true;
}

bool read_u32(std::uint32_t address, std::uint32_t &value) noexcept {
  return read_guest(address, &value, sizeof value);
}

bool read_u16(std::uint32_t address, std::uint16_t &value) noexcept {
  return read_guest(address, &value, sizeof value);
}

bool read_u8(std::uint32_t address, std::uint8_t &value) noexcept {
  return read_guest(address, &value, sizeof value);
}

// The PGL context object's sparse spans (coregs3_scene.hpp kPglSpans).
void copy_pgl_words(std::uint32_t (&words)[kPglWords]) noexcept {
  std::uint32_t written = 0;
  for (const PglSpan &span : kPglSpans) {
    for (std::uint32_t index = 0; index < span.words; ++index) {
      std::uint32_t value = 0;
      if (!read_u32(kPglContext + span.offset + index * 4u, value)) value = 0;
      if (written < kPglWords) words[written++] = value;
    }
  }
}

void copy_vf(const ps2::Cpu &cpu, std::uint32_t (&rows)[8][4]) noexcept {
  for (std::uint32_t row = 0; row < 8; ++row)
    for (std::uint32_t lane = 0; lane < 4; ++lane) rows[row][lane] = cpu.vu0.vf[24 + row].w[lane];
}

// func_0049CEA0 resolves the item's 9-bit texture and 7-bit material index into a record address. The indices travel in
// the item's `event` halfword, so the capture does not need the per-item callback hook: it resolves the same addresses
// from the same guest words (record bases and external cursors included).
struct RecordTables {
  std::uint32_t texture_array = 0;
  std::uint32_t material_array = 0;
  std::uint32_t external_texture_array = 0;
  std::uint32_t external_material_array = 0;
  std::uint32_t texture_cursor = 0;
  std::uint32_t material_cursor = 0;
};

RecordTables read_record_tables() noexcept {
  RecordTables tables;
  read_u32(kTextureArray, tables.texture_array);
  read_u32(kMaterialArray, tables.material_array);
  read_u32(kExternalTextureArray, tables.external_texture_array);
  read_u32(kExternalMaterialArray, tables.external_material_array);
  read_u32(kTextureCursor, tables.texture_cursor);
  read_u32(kMaterialCursor, tables.material_cursor);
  return tables;
}

std::uint32_t texture_record_address(const RecordTables &tables, std::uint16_t event) noexcept {
  std::uint32_t index = event & kTextureIndexMask;
  if (index == kExternalTextureIndex) index += tables.texture_cursor;
  if (index == 0) return 0;
  if (index >= kExternalTextureIndex) {
    if (tables.external_texture_array == 0) return 0;
    return tables.external_texture_array + tables.texture_cursor * kTextureRecordBytes;
  }
  if (tables.texture_array == 0) return 0;
  return tables.texture_array + index * kTextureRecordBytes - kTextureRecordBytes;
}

std::uint32_t material_record_address(const RecordTables &tables, std::uint16_t event) noexcept {
  std::uint32_t index = (event >> kMaterialIndexShift) & kMaterialIndexMask;
  if (index == kExternalMaterialIndex) index += tables.material_cursor;
  if (index == 0) return 0;
  if (index >= kExternalMaterialIndex) {
    if (tables.external_material_array == 0) return 0;
    return tables.external_material_array + tables.material_cursor * kMaterialRecordBytes;
  }
  if (tables.material_array == 0) return 0;
  return tables.material_array + index * kMaterialRecordBytes - kMaterialRecordBytes;
}

std::uint32_t find_texture(const FrameSnapshot &frame, std::uint32_t address) noexcept {
  for (std::uint32_t index = 0; index < frame.texture_count; ++index)
    if (frame.textures[index].address == address) return index;
  return kNoRecord;
}

std::uint32_t find_material(const FrameSnapshot &frame, std::uint32_t address) noexcept {
  for (std::uint32_t index = 0; index < frame.material_count; ++index)
    if (frame.materials[index].address == address) return index;
  return kNoRecord;
}

void mark_overflow(std::uint64_t flag) noexcept {
  g.frame_overflow = true;
  g.frame_overflow_flags |= flag;
  g.counters.overflow |= flag;
}

void reset_frame() noexcept {
  g.frame.draw_count = 0;
  g.frame.texture_count = 0;
  g.frame.material_count = 0;
  g.frame.draws.clear();
  g.frame.textures.clear();
  g.frame.materials.clear();
  g.frame.packets.clear();
  g.frame_overflow = false;
  g.frame_overflow_flags = 0;
  g.sequence = 0;
  g.pending = false;
}

bool frame_is_in_capture_range(std::uint64_t vblank) noexcept {
  if (!g.capture_range) return true;
  return vblank >= g.capture_first && vblank <= g.capture_last;
}

void finish_frame() noexcept {
  g.counters.frames += 1;
  if (g.trace_frames != 0 && g.counters.frames <= g.trace_frames)
    std::fprintf(stderr, "[coregs3] frame %llu vblank %llu draws %u textures %u packets %zu\n",
                 static_cast<unsigned long long>(g.counters.frames),
                 static_cast<unsigned long long>(g.frame_vblank), g.frame.draw_count, g.frame.texture_count,
                 g.frame.packets.size());
  const bool wanted = g.capture != nullptr && frame_is_in_capture_range(g.frame_vblank) &&
                      g.counters.frames_written < g.capture_max_frames;
  if (g.frame_overflow) {
    g.counters.refused_frames += 1;
    return;
  }
  if (!wanted) return;
  g.frame.index = g.frame_index;
  g.frame.vblank = g.frame_vblank;
  g.frame.overflow_flags = 0;
  g.frame.camera = g.camera;
  g.block.clear();
  if (!frame_block_bytes(g.frame, g.block)) {
    g.counters.refused_frames += 1;
    return;
  }
  if (std::fwrite(g.block.data(), 1, g.block.size(), g.capture) != g.block.size()) {
    std::fprintf(stderr, "[coregs3] capture write failed at frame %u; capture closed\n", g.frame_index);
    std::fclose(g.capture);
    g.capture = nullptr;
    return;
  }
  g.counters.bytes_written += g.block.size();
  g.counters.frames_written += 1;
  g.frame_index += 1;
}

void read_camera_call(ps2::Cpu &cpu, CameraSnapshot &camera) noexcept {
  // func_0033EAB8: a0 = base + (s16)*(u32*)base, with `base` in s2 (r18) and the low halfword of the pointed word.
  const std::uint32_t base = cpu.r[18].w[0];
  std::uint32_t table = 0;
  std::uint16_t offset = 0;
  if (!read_u32(base, table) || !read_u16(table, offset)) return;
  camera.camera = base + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(offset)));
  camera.valid |= 4u;
}

void copy_pgl_block(std::uint32_t (&words)[16]) noexcept {
  for (std::uint32_t index = 0; index < 16; ++index) {
    std::uint32_t value = 0;
    if (!read_u32(kPglContext + kObjMatrix + index * 4u, value)) value = 0;
    words[index] = value;
  }
}

}  // namespace

Mode mode() noexcept { return g.mode; }
bool active() noexcept { return g.mode != Mode::off; }
const CaptureCounters &counters() noexcept { return g.counters; }
const CameraSnapshot &current_camera() noexcept { return g.camera; }

void configure_from_environment() {
  if (g.configured) return;
  g.configured = true;
  const char *mode_text = std::getenv("GT4_COREGS3");
  g.mode = parse_mode(mode_text);
  if (mode_text != nullptr && *mode_text != '\0' && std::strcmp(mode_text, mode_name(g.mode)) != 0) {
    std::fprintf(stderr, "[coregs3] GT4_COREGS3=%s is not off, shadow or overlay: refusing to start\n", mode_text);
    std::exit(2);
  }
  if (g.mode == Mode::overlay) {
    std::fprintf(stderr, "[coregs3] GT4_COREGS3=overlay: the live overlay seam is not in this build; use shadow\n");
    std::exit(2);
  }
  if (g.mode == Mode::off) return;

  std::atexit([] { shutdown(); });

  g.frame.draws.reserve(kMaxDrawsPerFrame);
  g.frame.textures.reserve(kMaxTextureRecords);
  g.frame.materials.reserve(kMaxMaterialRecords);
  g.frame.packets.reserve(1u << 20);
  g.block.reserve(1u << 21);

  const char *range = std::getenv("GT4_COREGS3_CAPTURE_VBLANKS");
  if (range != nullptr && *range != '\0') {
    char *end = nullptr;
    const long long first = std::strtoll(range, &end, 10);
    if (end != nullptr && *end == '-') {
      const long long last = std::strtoll(end + 1, nullptr, 10);
      if (first >= 0 && last >= first) {
        g.capture_first = static_cast<std::uint64_t>(first);
        g.capture_last = static_cast<std::uint64_t>(last);
        g.capture_range = true;
      }
    }
    if (!g.capture_range)
      std::fprintf(stderr, "[coregs3] GT4_COREGS3_CAPTURE_VBLANKS=%s is not a-b; capturing every frame\n", range);
  }

  g.all_draws = std::getenv("GT4_COREGS3_ALL_DRAWS") != nullptr;
  configure_tap_from_environment();
  const char *path = std::getenv("GT4_COREGS3_CAPTURE");
  if (const char *trace = std::getenv("GT4_COREGS3_TRACE_FRAMES"); trace != nullptr && *trace != '\0')
    g.trace_frames = static_cast<std::uint64_t>(std::strtoll(trace, nullptr, 10));
  if (path != nullptr && *path != '\0') {
    if (const char *max_frames = std::getenv("GT4_COREGS3_CAPTURE_MAX_FRAMES"); max_frames != nullptr && *max_frames != '\0') {
      const long long value = std::strtoll(max_frames, nullptr, 10);
      if (value > 0) g.capture_max_frames = static_cast<std::uint64_t>(value);
    }
    g.capture = std::fopen(path, "wb");
    if (g.capture == nullptr) {
      std::fprintf(stderr, "[coregs3] cannot open capture %s: %s\n", path, std::strerror(errno));
      std::exit(2);
    }
    std::vector<std::uint8_t> header;
    write_scene_header(header, SceneHeader{});
    if (std::fwrite(header.data(), 1, header.size(), g.capture) != header.size()) {
      std::fprintf(stderr, "[coregs3] capture header write failed\n");
      std::exit(2);
    }
  }
  std::fprintf(stderr, "[coregs3] mode %s%s%s%s\n", mode_name(g.mode), g.capture != nullptr ? ", capture " : "",
               g.capture != nullptr ? path : "", g.capture_range ? " (VBlank range)" : "");
}

void shutdown() {
  tap_shutdown();
  if (g.capture != nullptr) {
    for (const std::vector<std::uint8_t> &block : g.pending_blocks)
      if (std::fwrite(block.data(), 1, block.size(), g.capture) != block.size()) {
        std::fprintf(stderr, "[coregs3] capture write failed while flushing\n");
        break;
      }
    g.pending_blocks.clear();
    std::fclose(g.capture);
    g.capture = nullptr;
  }
  if (g.mode != Mode::off) print_report(stderr);
}

void at_vblank(std::uint64_t vblank) noexcept {
  if (g.mode == Mode::off) return;
  // The donor calls this at the start of every guest VBlank. The frame the guest has just finished is closed here, so a
  // capture holds exactly the draws of one VBlank-to-VBlank interval whatever the guest's render ordering is.
  if (g.frame_open) {
    g.frame_open = false;
    g.frame_vblank = vblank > 0 ? vblank - 1 : 0;
    finish_frame();
  }
  reset_frame();
  g.frame_open = true;
  g.vblank = vblank;
  tap_at_vblank(vblank);
  if (vblank != 0 && vblank % 600 == 0) print_report(stderr);
}

void car_pass_begin(ps2::Cpu &cpu) noexcept {
  (void)cpu;
  if (g.mode == Mode::off) return;
  // TASK-090: name the pass in the worker's job stream. Flushing here and at the end puts the pass's VIF packets in their
  // own jobs, so the tap's records of this pass can be selected exactly (no guessing from vertex counts).
  gt4::mtvu::flush_vif_chain();
  g.pass_job_first = gt4::mtvu::last_posted_job() + 1;
  g.in_car_pass = true;
  g.pass_count += 1;
  if (g.trace_frames != 0 && g.trace_events < g.trace_frames) {
    std::uint32_t vtable = 0, target_380 = 0, target_3d8 = 0, target_3e0 = 0;
    read_u32(cpu.r[17].w[0] + 100u, vtable);
    // A method entry is { short d, short i, void *fn }: the function pointer sits at +4.
    read_u32(vtable + 0x380u + 4u, target_380);
    read_u32(vtable + 0x3D8u + 4u, target_3d8);
    read_u32(vtable + 0x3E0u + 4u, target_3e0);
    std::fprintf(stderr, "[coregs3] pass begin %llu self %08X vt %08X cars(+380) %08X (+3D8) %08X (+3E0) %08X\n",
                 static_cast<unsigned long long>(g.vblank), cpu.r[17].w[0], vtable, target_380, target_3d8,
                 target_3e0);
  }
}

void car_pass_end(ps2::Cpu &cpu) noexcept {
  (void)cpu;
  if (g.mode == Mode::off) return;
  if (!g.in_car_pass) {
    g.orphan_pass_ends += 1;
    return;
  }
  g.in_car_pass = false;
  gt4::mtvu::flush_vif_chain();
  g.pass_job_last = gt4::mtvu::last_posted_job();
  if (g.trace_frames != 0 && g.trace_events < g.trace_frames) {
    std::fprintf(stderr, "[coregs3] pass end %llu draws %u jobs %llu..%llu\n",
                 static_cast<unsigned long long>(g.vblank), g.frame.draw_count,
                 static_cast<unsigned long long>(g.pass_job_first),
                 static_cast<unsigned long long>(g.pass_job_last));
    ++g.trace_events;
  }
}

void projection_set(ps2::Cpu &cpu) noexcept {
  if (g.mode == Mode::off) return;
  g.camera.valid |= 1u;
  copy_pgl_block(g.camera.pgl_block_k3);
  read_camera_call(cpu, g.camera);
}

void modelview_set(ps2::Cpu &cpu) noexcept {
  if (g.mode == Mode::off) return;
  g.camera.valid |= 2u;
  copy_pgl_block(g.camera.pgl_block_k4);
  copy_vf(cpu, g.camera.vf_k4);
  read_camera_call(cpu, g.camera);
}

void before_shape_call(ps2::Cpu &cpu) noexcept {
  if (g.mode == Mode::off) return;
  g.counters.calls += 1;
  if (!g.in_car_pass && !g.all_draws) {
    g.counters.outside_pass += 1;
    return;
  }
  if (g.frame_overflow) return;  // one refusal per frame: keep counting, store nothing
  if (g.frame.draw_count >= kMaxDrawsPerFrame) {
    mark_overflow(kOverflowDraws);
    return;
  }

  CapturedDraw draw;
  draw.sequence = g.sequence++;
  draw.pass = 1;  // cars
  draw.shape = cpu.r[4].w[0];
  if (draw.shape == 0) {
    g.counters.guest_read_failures += 1;
    return;
  }
  std::uint8_t flag = 0, bits = 0;
  std::uint16_t count = 0, nc = 0, nd = 0;
  if (!read_u32(draw.shape + 0x00u, draw.cmd_a) || !read_u32(draw.shape + kCmdValue, draw.cmd_value) ||
      !read_u8(draw.shape + kCmdFlag, flag) || !read_u8(draw.shape + kCmdBits, bits) ||
      !read_u16(draw.shape + kCmdCount, count) || !read_u16(draw.shape + kCmdNc, nc) ||
      !read_u16(draw.shape + kCmdNd, nd)) {
    g.counters.guest_read_failures += 1;
    return;
  }
  draw.cmd_flag = flag;
  draw.cmd_bits = bits;
  draw.cmd_count = count;
  draw.cmd_nc = nc;
  draw.cmd_nd = nd;
  if (count > g.counters.largest_items) g.counters.largest_items = count;
  if (count > kMaxItemsPerDraw) {
    mark_overflow(kOverflowItems);
    return;
  }

  copy_pgl_words(draw.pgl);
  copy_vf(cpu, draw.vf);

  const RecordTables tables = read_record_tables();
  g.pending = true;
  g.pending_sequence = draw.sequence;
  g.pending_out = draw.pgl[42];  // kPglSpans: 20 words at +0x00, then +0x1A0..; 0x1F8 is index 42
  g.pending_draw = g.frame.draw_count;

  std::uint32_t copied_bytes = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    CapturedItem &item = draw.items[index];
    const std::uint32_t at = draw.shape + kCmdItems + index * kCmdItemBytes;
    std::uint32_t address = 0;
    std::uint16_t qwords = 0, event = 0;
    if (!read_u32(at, address) || !read_u16(static_cast<std::uint32_t>(at + 4), qwords) ||
        !read_u16(static_cast<std::uint32_t>(at + 6), event)) {
      g.counters.guest_read_failures += 1;
      draw.item_count = index;
      break;
    }
    item.address = address & 0x0FFFFFFFu;
    item.qwords = qwords;
    item.event = event;

    const std::uint32_t texture_address = texture_record_address(tables, event);
    if (texture_address != 0) {
      std::uint32_t slot = find_texture(g.frame, texture_address);
      if (slot == kNoRecord) {
        if (g.frame.texture_count >= kMaxTextureRecords) {
          mark_overflow(kOverflowTextures);
        } else {
          CapturedTexture texture;
          texture.address = texture_address;
          if (read_guest(texture_address, texture.bytes, kTextureRecordBytes)) {
            slot = g.frame.texture_count++;
            g.frame.textures.push_back(texture);
            g.counters.records += 1;
          } else {
            g.counters.guest_read_failures += 1;
          }
        }
      }
      item.texture_record = slot;
    }

    const std::uint32_t material_address = material_record_address(tables, event);
    if (material_address != 0) {
      std::uint32_t slot = find_material(g.frame, material_address);
      if (slot == kNoRecord) {
        if (g.frame.material_count >= kMaxMaterialRecords) {
          mark_overflow(kOverflowMaterials);
        } else {
          CapturedMaterial material;
          material.address = material_address;
          if (read_guest(material_address, material.bytes, kMaterialRecordBytes)) {
            slot = g.frame.material_count++;
            g.frame.materials.push_back(material);
            g.counters.records += 1;
          } else {
            g.counters.guest_read_failures += 1;
          }
        }
      }
      item.material_record = slot;
    }

    const std::uint32_t packet_bytes = static_cast<std::uint32_t>(qwords) * 16u;
    if (packet_bytes != 0) {
      if (packet_bytes > kMaxPacketBytesPerDraw || copied_bytes + packet_bytes > kMaxPacketBytesPerDraw ||
          g.frame.packets.size() + packet_bytes > kMaxPacketBytesPerFrame) {
        mark_overflow(kOverflowPackets);
      } else {
        const std::size_t offset = g.frame.packets.size();
        g.frame.packets.resize(offset + packet_bytes);
        if (read_guest(item.address, g.frame.packets.data() + offset, packet_bytes)) {
          item.packet_offset = static_cast<std::uint32_t>(offset);
          item.packet_bytes = packet_bytes;
          copied_bytes += packet_bytes;
          g.counters.item_packets += 1;
        } else {
          g.frame.packets.resize(offset);
          g.counters.guest_read_failures += 1;
          mark_overflow(kOverflowGuestRead);
        }
      }
    }
  }
  draw.item_count = count;
  if (copied_bytes > g.counters.largest_draw_bytes) g.counters.largest_draw_bytes = copied_bytes;
  if (g.frame.packets.size() > g.counters.largest_frame_bytes)
    g.counters.largest_frame_bytes = static_cast<std::uint32_t>(g.frame.packets.size());
  g.frame.draws.push_back(draw);
  g.frame.draw_count = static_cast<std::uint32_t>(g.frame.draws.size());
  g.counters.captured += 1;
}

void after_shape_call(ps2::Cpu &cpu) noexcept {
  (void)cpu;
  if (g.mode == Mode::off) return;
  if (!g.pending) return;
  g.pending = false;
  // func_0049D328 starts by writing the matrix packet at the PGL's output cursor (func_0049E548), so the eight qwords at
  // that address are what VU1 receives at 0xC1 for this draw. Reading them back after the call avoids guessing which
  // VU0 register held which row at the pre-call instant.
  if (g.pending_draw < g.frame.draws.size() && g.pending_out != 0) {
    CapturedDraw &draw = g.frame.draws[g.pending_draw];
    if (read_guest(g.pending_out, draw.matrix_packet, sizeof draw.matrix_packet))
      draw.matrix_packet_valid = 1;
  }
  g.pending_draw = kNoRecord;
  g.pending_out = 0;
}

void print_report(std::FILE *out) noexcept {
  if (out == nullptr) return;
  const CaptureCounters &c = g.counters;
  std::fprintf(out,
               "[coregs3] mode %s frames %llu written %llu refused %llu calls %llu captured %llu outside %llu packets %llu "
               "records %llu bytes %llu reads_failed %llu overflow 0x%llx vblank %llu max_items %u max_draw %u "
               "max_frame %u passes %llu orphan_ends %llu\n",
               mode_name(g.mode), static_cast<unsigned long long>(c.frames),
               static_cast<unsigned long long>(c.frames_written),
               static_cast<unsigned long long>(c.refused_frames), static_cast<unsigned long long>(c.calls),
               static_cast<unsigned long long>(c.captured), static_cast<unsigned long long>(c.outside_pass),
               static_cast<unsigned long long>(c.item_packets), static_cast<unsigned long long>(c.records),
               static_cast<unsigned long long>(c.bytes_written), static_cast<unsigned long long>(c.guest_read_failures),
               static_cast<unsigned long long>(c.overflow), static_cast<unsigned long long>(g.vblank), c.largest_items,
               c.largest_draw_bytes, c.largest_frame_bytes, static_cast<unsigned long long>(g.pass_count),
               static_cast<unsigned long long>(g.orphan_pass_ends));
  const TapCounters &tap = tap_counters();
  if (tap.records != 0)
    std::fprintf(out, "[coregs3] tap records %llu bytes %llu refused %llu\n",
                 static_cast<unsigned long long>(tap.records), static_cast<unsigned long long>(tap.bytes),
                 static_cast<unsigned long long>(tap.refused));
}

}  // namespace gt4::coregs3
