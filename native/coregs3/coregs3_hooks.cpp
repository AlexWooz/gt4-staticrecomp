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

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "coregs3_chain.hpp"
#include "coregs3_gsstream.hpp"
#include "coregs3_live_world.hpp"
#include "coregs3_present.hpp"
#include "coregs3_tap.hpp"
#include "coregs3_texwin.hpp"
#include "coregs3_vram.hpp"
#include "mtvu_runtime.hpp"

#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "vif.hpp"  // ps2::vif::read: the EE-side shadow VIF1 state (BASE/OFFSET/TOPS/dbf) for the report

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
  bool world_draws = false;  // live world: store the car pass and the sky/far-underlay/course/reflection passes only
  // Format version 5: the pass the markers currently name and the open journal entry (index into
  // FrameSnapshot::pass_states), kNoRecord when no pass is open.
  std::uint32_t current_pass = kPassUnknown;
  std::uint32_t open_pass = kNoRecord;
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
  // Format version 4: the last UNPACK to TOPS+0xC9 in VIF command order, tracked across every hooked shape call so a
  // draw that uploads nothing records the block the VU1 actually keeps. `chain_seen`/`last_post_out` detect a PGL
  // output-cursor discontinuity (another writer appended to or rewound the buffer), which makes the carried block
  // uncertain until the next observed upload.
  CarriedLight carried_light;
  bool chain_seen = false;
  std::uint32_t last_post_out = 0;
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

bool read_u64(std::uint32_t address, std::uint64_t &value) noexcept {
  return read_guest(address, &value, sizeof value);
}

// The scene object of func_0033EAB8: `course = *(*(*(self + 0x6C) + 0x80) + 4)`, `scene = *(course + 0x80)`. A zero
// pointer anywhere means the object has no scene in this frame (menus); that is not a read failure.
bool read_scene(std::uint32_t self, std::uint32_t &scene) noexcept {
  std::uint32_t a = 0, b = 0, course = 0;
  if (!read_u32(self + 0x6Cu, a)) return false;
  if (a == 0) return false;
  if (!read_u32(a + 0x80u, b)) {
    g.counters.environment_read_failures += 1;
    return false;
  }
  if (b == 0) return false;
  if (!read_u32(b + 4u, course)) {
    g.counters.environment_read_failures += 1;
    return false;
  }
  if (course == 0) return false;
  if (!read_u32(course + 0x80u, scene)) {
    g.counters.environment_read_failures += 1;
    return false;
  }
  return scene != 0;
}

// The scene environment block (format version 5), copied once per frame at the first world pass begin.
void capture_environment(ps2::Cpu &cpu) noexcept {
  if (g.frame.environment.valid != 0) return;
  std::uint32_t scene = 0;
  if (!read_scene(cpu.r[17].w[0], scene)) return;
  std::uint32_t index = 0;
  for (const EnvSpan &span : kEnvSpans) {
    for (std::uint32_t word = 0; word < span.words; ++word) {
      std::uint32_t value = 0;
      if (!read_u32(scene + span.offset + word * 4u, value)) {
        g.counters.environment_read_failures += 1;
        return;
      }
      if (index < kEnvWords) g.frame.environment.words[index++] = value;
    }
  }
  g.frame.environment.valid = 1;
  g.counters.environment_frames += 1;
}

// The PGL GS register mirrors at a pass marker, in the `.gt4gs` pass-record order: the seven the v5 journal carries
// (FOGCOL, FRAME, XYOFFSET, ZBUF, SCISSOR, ALPHA, TEST) plus CLAMP 0x1A8 and TEX1 0x1B8. All raw 64-bit words.
bool read_gs_registers(std::uint64_t (&words)[kGsStreamPassRegisters]) noexcept {
  bool ok = true;
  ok &= read_u64(kPglContext + kGsShadowFogCol, words[0]);
  ok &= read_u64(kPglContext + kGsShadowFrame, words[1]);
  ok &= read_u64(kPglContext + kGsShadowXYOffset, words[2]);
  ok &= read_u64(kPglContext + kGsShadowZBuf, words[3]);
  ok &= read_u64(kPglContext + kGsShadowScissor, words[4]);
  ok &= read_u64(kPglContext + kGsShadowAlpha, words[5]);
  ok &= read_u64(kPglContext + kGsShadowTest, words[6]);
  ok &= read_u64(kPglContext + kGsShadowClamp, words[7]);
  ok &= read_u64(kPglContext + kGsShadowTex1, words[8]);
  return ok;
}

// The PGL GS state shadow at a pass begin (format version 5), the first seven words of the marker read. The `.gt4gs`
// pass record carries all nine; both come from the same guest read, so the two files agree by construction.
void capture_gs_shadow(CapturedPassState &state, const std::uint64_t (&words)[kGsStreamPassRegisters]) noexcept {
  state.fogcol = words[0];
  state.frame = words[1];
  state.xyoffset = words[2];
  state.zbuf = words[3];
  state.scissor = words[4];
  state.alpha = words[5];
  state.test = words[6];
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

// Format version 4: the per-item light state and run pair come from walking the PGL output chain
// (native/coregs3/coregs3_chain.cpp). The walker runs for every hooked shape call, in or out of the pass, because the
// carried TOPS+0xC9 state is global to the VIF command stream. The per-draw counters keep the version 2/3 meaning:
// `light_blocks` is the first item's upload, `light_blocks_missing` a draw with a nonzero Cmd flag and no such upload.
void account_chain(const CapturedDraw *draw, const ChainWalkCounters &walk) noexcept {
  g.counters.chain_calls += 1;
  if (!walk.complete) g.counters.chain_incomplete += 1;
  if (draw == nullptr) return;
  g.counters.light_items_uploaded += walk.items_uploaded;
  g.counters.light_items_carried += walk.items_carried;
  g.counters.light_items_uncertain += walk.items_uncertain;
  g.counters.light_items_none += walk.items_none;
  g.counters.program_items += walk.items_run;
  g.counters.program_items_missing += walk.items_run_missing;
  g.counters.program_item_variants += walk.item_variants;
  g.counters.program_item_mscal_variants += walk.item_mscal_variants;
  if (draw->program_valid != 0)
    g.counters.program_selectors += 1;
  else
    g.counters.program_selectors_missing += 1;
  // Format version 6: the morph factor upload segments this chain carried (the draw pointer may be null: the uploads of
  // calls outside the stored passes are still counted, exactly like the light block).
  g.counters.morph_uploads += walk.morph_tags;
  g.counters.morph_value_mismatches += walk.morph_mismatch_values;
  if (draw->cmd_flag != 0) {
    if (draw->light_block_qwords != 0)
      g.counters.light_blocks += 1;
    else
      g.counters.light_blocks_missing += 1;
  }
  bool uploaded = false, known = false;
  for (std::uint32_t index = 0; index < draw->item_count; ++index) {
    const std::uint32_t source = draw->items[index].light_source;
    if (source == kLightItemUploaded) uploaded = true;
    if (source != kLightItemNone) known = true;
  }
  if (!uploaded) {
    if (known)
      g.counters.light_blocks_carried += 1;
    else
      g.counters.light_blocks_none += 1;
  }
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
  g.frame.pass_states.clear();
  g.frame.environment = SceneEnvironment{};
  g.frame_overflow = false;
  g.frame_overflow_flags = 0;
  g.sequence = 0;
  g.pending = false;
  g.current_pass = kPassUnknown;
  g.open_pass = kNoRecord;
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
  g.frame.index = g.frame_index;
  g.frame.vblank = g.frame_vblank;
  g.frame.overflow_flags = 0;
  g.frame.camera = g.camera;
  // `present` (docs/COREGS3-HOOKS.md §10.3): publish the complete frame for the live renderer. It needs no capture file
  // and never touches the guest; the O(1) swap keeps the buffers for the next VBlank. Only `shadow` writes the file.
  if (g.mode == Mode::present) {
    g.frame.index = static_cast<std::uint32_t>(g.counters.frames);
    present::publish(g.frame);
  }
  if (!wanted) return;
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
    std::fprintf(stderr, "[coregs3] GT4_COREGS3=%s is not off, shadow, overlay or present: refusing to start\n", mode_text);
    std::exit(2);
  }
  if (g.mode == Mode::overlay) {
    std::fprintf(stderr, "[coregs3] GT4_COREGS3=overlay: the live overlay seam is not in this build; use shadow\n");
    std::exit(2);
  }
  present::set_active(g.mode == Mode::present);
  if (g.mode == Mode::present) {
    // TASK-090 live timeline: the presentation rate and mode. An unusable value stops the start here, like an unknown
    // GT4_COREGS3, so a run never silently presents at a rate the owner did not ask for.
    present::TimelineConfig timeline;
    std::string timeline_error;
    if (!present::timeline_config_from_environment(timeline, timeline_error)) {
      std::fprintf(stderr, "[coregs3] %s: refusing to start\n", timeline_error.c_str());
      std::exit(2);
    }
    present::set_timeline_config(timeline);
    std::fprintf(stderr, "[coregs3] present timeline: %u Hz, mode %s%s\n", timeline.hz,
                 presentation_mode_name(timeline.mode),
                 timeline.selected_tick == UINT32_MAX ? "" : ", diagnostic single-tick image");
  }
  if (g.mode == Mode::off) return;

  std::atexit([] { shutdown(); });

  // Format version 4 gives every item its own 16-qword light block, so a draw record is ~80 KB. The car pass measured
  // ~116 draws per frame; reserve a realistic frame and let the vector grow geometrically if a scene exceeds it (the
  // kMaxDrawsPerFrame cap is still enforced by before_shape_call, not by this reservation).
  g.frame.draws.reserve(512);
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
  // TASK-090 live world: in `present` the renderer draws the captured sky/far-underlay/course passes over the composed
  // image, so the ring must carry them. The live world reads GT4_COREGS3_LIVE_WORLD (default on); its per-job texture
  // store is fed by the sink the MTVU runtime forwards every posted item packet to (the same site as the tap's source
  // record, available at post time instead of several VBlanks later). `shadow` and `off` never reach this branch.
  if (g.mode == Mode::present) {
    live_world::configure_from_environment();
    if (live_world::active()) {
      set_live_job_source_sink(&live_world::note_job_source);
      g.world_draws = true;
      std::fprintf(stderr, "[coregs3] live world: on (captured sky/far-underlay/course passes, per-job textures)\n");
    } else {
      std::fprintf(stderr, "[coregs3] live world: off (GT4_COREGS3_LIVE_WORLD=0): the car pass only\n");
    }
  }
  configure_tap_from_environment();
  // TASK-090 S1: the `.gt4gs` PATH2/PATH3 stream is a separate sidecar with its own switch and bounds; it is configured
  // only here, so the writer is one branch in builds without the hooks. `GT4_COREGS3=off` never reaches this point.
  configure_gsstream_from_environment();
  const char *path = std::getenv("GT4_COREGS3_CAPTURE");
  if (const char *trace = std::getenv("GT4_COREGS3_TRACE_FRAMES"); trace != nullptr && *trace != '\0')
    g.trace_frames = static_cast<std::uint64_t>(std::strtoll(trace, nullptr, 10));
  // Only `shadow` writes the .gt4scene capture: `present` reads the published ring and needs no file.
  if (path != nullptr && *path != '\0' && g.mode != Mode::shadow) {
    std::fprintf(stderr, "[coregs3] GT4_COREGS3_CAPTURE is ignored in mode %s; only shadow writes a capture\n",
                 mode_name(g.mode));
    path = nullptr;
  }
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
  // TASK-090 S1: closes the open frame and writes the preallocated ring to the `.gt4gs` file, off the guest's timeline.
  gsstream_shutdown();
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
  gsstream_frame(vblank);
  if (vblank != 0 && vblank % 600 == 0) print_report(stderr);
}

namespace {

// The draw marker point: `pre` (the default) is before the shape call, `post` after it returns. The schedule of the
// texture uploads decides which one sees a draw's own pixels; GT4_COREGS3_VRAM_DRAW_AT=post is the diagnostic.
bool draw_mark_at_post() noexcept {
  static const bool post = [] {
    const char *value = std::getenv("GT4_COREGS3_VRAM_DRAW_AT");
    return value != nullptr && std::strcmp(value, "post") == 0;
  }();
  return post;
}

// TASK-090 pass textures: the GS blocks a draw's texture records name (the shared rule is coregs3_texwin.hpp). TASK-090
// texture mip: a record with TEX1.MXL > 0 also names its mip levels through MIPTBP1/MIPTBP2, and the snapshot must carry
// them or the offline decoder samples whatever later upload sits at the level address.
void collect_draw_windows(const FrameSnapshot &frame, const CapturedDraw &draw, vram::Window *windows,
                          std::uint32_t &count) noexcept {
  for (std::uint32_t index = 0; index < draw.item_count; ++index) {
    const std::uint32_t slot = draw.items[index].texture_record;
    if (slot == kNoRecord || slot >= frame.textures.size()) continue;
    const std::uint8_t *bytes = frame.textures[slot].bytes;
    texture_windows_from_state(vram::read_u64(bytes, 0), vram::read_u64(bytes, 8), vram::read_u64(bytes, 16),
                               vram::read_u64(bytes, 24), windows, count);
  }
}

}  // namespace

void pass_begin(ps2::Cpu &cpu, std::uint32_t pass) noexcept {
  if (g.mode == Mode::off) return;
  if (pass >= kPassCount) pass = kPassUnknown;
  // TASK-090 pass textures: hand the marker to the GS stream in stream order, so the SLM snapshot at a pass begin is the
  // content the pass's own draws sample, not a later frame-end state (docs/COREGS3-WORLD-MATERIALS.md). One atomic load
  // when no VRAM capture is configured; the snapshot itself runs on the GS side.
  vram::note_pass_begin(pass, g.vblank);
  // The scene environment is read through the RacePS2Base object in r17. Only the markers of func_0033EAB8 (sky and
  // later) run with that register; the env-map marker is in the earlier world setup and the panel marker in
  // RacePS2Base__render, where r17 holds other pointers, so neither touches the environment.
  if (pass >= kPassSky && pass != kPassPanel) capture_environment(cpu);
  if (g.current_pass != kPassUnknown) g.counters.pass_mismatched_ends += 1;  // nested marker: keep the outer one
  g.current_pass = pass;
  // TASK-090 S1: the `.gt4gs` pass record, from the same nine-word guest read the journal uses below for its first seven.
  // It is written before the journal's overflow checks: the stream is a separate sidecar with its own bounds, and a scene
  // frame the draw capture refuses must not take the stream's pass records with it.
  std::uint64_t registers[kGsStreamPassRegisters] = {};
  if (!read_gs_registers(registers)) g.counters.guest_read_failures += 1;
  gsstream_pass_marker(pass, true, registers, gt4::mtvu::last_posted_job(), cpu.pc, cpu.r[31].w[0]);
  if (g.frame_overflow) return;
  if (g.frame.pass_states.size() >= kMaxPassStatesPerFrame) {
    mark_overflow(kOverflowPassStates);
    return;
  }
  CapturedPassState state;
  state.pass = pass;
  state.vblank = g.vblank;
  capture_gs_shadow(state, registers);
  g.frame.pass_states.push_back(state);
  g.open_pass = static_cast<std::uint32_t>(g.frame.pass_states.size() - 1);
  g.counters.pass_states += 1;
}

void pass_end(ps2::Cpu &cpu, std::uint32_t pass) noexcept {
  if (g.mode == Mode::off) return;
  if (g.current_pass == kPassUnknown) {
    g.counters.pass_orphan_ends += 1;
    return;
  }
  if (g.current_pass != pass) {
    g.counters.pass_mismatched_ends += 1;
    return;  // do not close an entry a different marker opened
  }
  // TASK-090 S1: the `.gt4gs` pass_end record, from the nine-word guest read at this marker; the recorder closes only the
  // pass its own begin opened, so the stream's attribution follows the journal's rule.
  std::uint64_t registers[kGsStreamPassRegisters] = {};
  if (!read_gs_registers(registers)) g.counters.guest_read_failures += 1;
  gsstream_pass_marker(pass, false, registers, gt4::mtvu::last_posted_job(), cpu.pc, cpu.r[31].w[0]);
  g.current_pass = kPassUnknown;
  g.open_pass = kNoRecord;
}

void car_pass_begin(ps2::Cpu &cpu) noexcept {
  if (g.mode == Mode::off) return;
  // TASK-090: name the pass in the worker's job stream. Flushing here and at the end puts the pass's VIF packets in their
  // own jobs, so the tap's records of this pass can be selected exactly (no guessing from vertex counts).
  gt4::mtvu::flush_vif_chain();
  g.pass_job_first = gt4::mtvu::last_posted_job() + 1;
  g.in_car_pass = true;
  g.pass_count += 1;
  pass_begin(cpu, kPassCars);
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
  if (g.mode == Mode::off) return;
  if (!g.in_car_pass) {
    g.orphan_pass_ends += 1;
    return;
  }
  g.in_car_pass = false;
  pass_end(cpu, kPassCars);
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

void before_shape_call(ps2::Cpu &cpu, std::uint32_t call_site) noexcept {
  if (g.mode == Mode::off) return;
  g.counters.calls += 1;
  // Format version 5: the pass comes from the active K7 marker; the direct call sites that do not pass through
  // ModelSet2___render carry their own pass. A call outside any marker stays `unknown` (counted, never guessed).
  std::uint32_t pass = g.current_pass;
  if (call_site == kCallSiteEnvMap)
    pass = kPassEnvMap;
  else if (call_site == kCallSiteTachometer)
    pass = kPassPanel;
  else if (call_site == kCallSiteBillboard && pass == kPassUnknown)
    pass = kPassBillboards;
  if (pass >= kPassCount) pass = kPassUnknown;
  // TASK-090 live world: `present` needs the car pass plus the world passes (sky, far underlay, course; the reflection
  // pass joins when it is drawn), not every shape call of a menu: an unmarked call is one of ~26 per menu frame and a
  // stored CapturedDraw is ~50 KB, so storing everything costs the guest thread without helping the picture.
  const bool world_pass = pass == kPassSky || pass == kPassFarUnderlay || pass == kPassCourse ||
                          pass == kPassReflection;
  const bool store = g.in_car_pass || g.all_draws || (g.world_draws && world_pass);
  if (!store) g.counters.outside_pass += 1;
  // Every hooked call writes a PGL output chain, even outside the car pass. Its uploads are part of the VU1's carried
  // TOPS+0xC9 state, so the post hook walks it whatever the pass (the draw pointer is null for an un-stored call).
  std::uint32_t pre_out = 0;
  if (!read_u32(kPglContext + kObjOut, pre_out)) pre_out = 0;  // kObjOut +0x1F8 is the PGL output cursor
  g.pending = true;
  g.pending_out = pre_out;
  g.pending_draw = kNoRecord;
  if (!store) return;
  if (g.frame_overflow) return;  // one refusal per frame: keep counting, walk the chain, store nothing
  if (g.frame.draw_count >= kMaxDrawsPerFrame) {
    mark_overflow(kOverflowDraws);
    return;
  }

  CapturedDraw draw;
  draw.sequence = g.sequence++;
  draw.pass = pass;
  draw.call_site = call_site;
  // The caller return address of ModelSet2___render: its frame saves ra at +168 (translated prologue L_00455708).
  if (call_site == kCallSiteModelSet2) read_u32(cpu.r[29].w[0] + 168u, draw.caller_ra);
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
  g.pending_sequence = draw.sequence;
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
  // TASK-090 pass textures: hand this draw's texture/palette blocks to the GS stream, so the copy lands after every GS
  // command posted before the draw and before the draw's own packets. One atomic load unless a VRAM capture is set.
  if (vram::snapshot_sink_installed() && vram::draw_marks_enabled() && !draw_mark_at_post()) {
    vram::Window windows[vram::kMaxCaptureWindows] = {};
    std::uint32_t window_count = 0;
    collect_draw_windows(g.frame, draw, windows, window_count);
    if (window_count != 0) vram::note_draw_textures(draw.sequence, g.vblank, windows, window_count);
  }
  g.counters.captured += 1;
  g.counters.pass_draws[pass] += 1;
  g.counters.pass_items[pass] += count;
  if (g.open_pass != kNoRecord && g.open_pass < g.frame.pass_states.size()) {
    CapturedPassState &state = g.frame.pass_states[g.open_pass];
    if (state.first_draw == kNoRecord) state.first_draw = draw.sequence;
    state.draw_count += 1;
    state.item_count += count;
  }
}

void after_shape_call(ps2::Cpu &cpu) noexcept {
  (void)cpu;
  if (g.mode == Mode::off) return;
  if (!g.pending) return;
  g.pending = false;
  const std::uint32_t pre_out = g.pending_out;
  const std::uint32_t draw_index = g.pending_draw;
  g.pending_draw = kNoRecord;
  g.pending_out = 0;

  CapturedDraw *draw = nullptr;
  if (draw_index != kNoRecord && draw_index < g.frame.draws.size()) draw = &g.frame.draws[draw_index];
  // func_0049D328 starts by writing the matrix packet at the PGL's output cursor (func_0049E548), so the eight qwords at
  // that address are what VU1 receives at 0xC1 for this draw. Reading them back after the call avoids guessing which
  // VU0 register held which row at the pre-call instant.
  if (draw != nullptr && pre_out != 0 && read_guest(pre_out, draw->matrix_packet, sizeof draw->matrix_packet))
    draw->matrix_packet_valid = 1;

  std::uint32_t post_out = 0;
  const bool have_cursor = pre_out != 0 && read_u32(kPglContext + kObjOut, post_out);
  // A cursor discontinuity means another writer appended to or rewound the PGL output buffer after the last chain this
  // capture walked, so an unseen TOPS+0xC9 upload may have followed the tracked block.
  if (g.chain_seen && g.last_post_out != 0 && (!have_cursor || pre_out != g.last_post_out)) {
    g.counters.chain_gaps += 1;
    g.carried_light.gap_epoch += 1;  // an unseen writer may have flipped the TOPS parity
  }
  g.chain_seen = true;

  // Diagnostic marker point (GT4_COREGS3_VRAM_DRAW_AT=post): the draw's texture blocks copied after the shape call.
  if (draw != nullptr && vram::snapshot_sink_installed() && vram::draw_marks_enabled() && draw_mark_at_post()) {
    vram::Window windows[vram::kMaxCaptureWindows] = {};
    std::uint32_t window_count = 0;
    collect_draw_windows(g.frame, *draw, windows, window_count);
    if (window_count != 0) vram::note_draw_textures(draw->sequence, g.vblank, windows, window_count);
  }

  ChainWalkCounters walk;
  bool complete = false;
  if (have_cursor) complete = walk_packet_chain(pre_out, post_out, read_guest, draw, g.carried_light, walk);
  account_chain(draw, walk);
  // Format version 6: the morph factor's context word, read directly as the cross-check of the chain upload. The source
  // of the VIF UNPACK 0x7001C0F2 is PGL +0xC30 (func_0049D1A0 reads self->xc30); the chain value is what the VU1 gets.
  if (draw != nullptr) {
    std::uint32_t pgl_factor = 0;
    if (read_u32(kPglContext + kMorphSource, pgl_factor)) {
      draw->morph_factor_pgl = pgl_factor;
      draw->morph_factor_valid |= kMorphPglRead;
      g.counters.morph_reads += 1;
    }
    if (draw->morph_uploads != 0) {
      draw->morph_factor_valid |= kMorphUploadSeen;
      g.counters.morph_draws += 1;
      if ((draw->morph_factor_valid & kMorphPglRead) != 0) {
        if (draw->morph_factor == draw->morph_factor_pgl)
          draw->morph_factor_valid |= kMorphValuesAgree;
        else
          g.counters.morph_mismatches += 1;
      }
    } else if ((draw->cmd_flag >> 5) == 1u || (draw->cmd_flag >> 5) == 2u) {
      // The decomp's remit rule (func_0049D1A0 writes the segment for Cmd kind >> 5 = 1 or 2): a draw it asks for and
      // the chain did not carry is a counted capture gap, never a guessed factor.
      g.counters.morph_missing += 1;
    }
  }
  if (complete) {
    g.last_post_out = post_out;
  } else {
    g.last_post_out = 0;
    g.carried_light.gap_epoch += 1;  // unparsed commands may follow
  }
}

void print_report(std::FILE *out) noexcept {
  if (out == nullptr) return;
  const CaptureCounters &c = g.counters;
  std::fprintf(out,
               "[coregs3] mode %s frames %llu written %llu refused %llu calls %llu captured %llu outside %llu packets %llu "
               "records %llu bytes %llu reads_failed %llu overflow 0x%llx vblank %llu max_items %u max_draw %u "
               "max_frame %u passes %llu orphan_ends %llu lights %llu lights_missing %llu selectors %llu "
               "selectors_missing %llu light_items %llu light_carried %llu light_uncertain %llu light_none %llu "
               "program_items %llu program_items_missing %llu program_variants %llu program_mscal_variants %llu "
               "chain_calls %llu chain_gaps %llu chain_incomplete %llu morph_uploads %llu morph_draws %llu "
               "morph_reads %llu morph_mismatches %llu morph_value_mismatches %llu morph_missing %llu\n",
               mode_name(g.mode), static_cast<unsigned long long>(c.frames),
               static_cast<unsigned long long>(c.frames_written),
               static_cast<unsigned long long>(c.refused_frames), static_cast<unsigned long long>(c.calls),
               static_cast<unsigned long long>(c.captured), static_cast<unsigned long long>(c.outside_pass),
               static_cast<unsigned long long>(c.item_packets), static_cast<unsigned long long>(c.records),
               static_cast<unsigned long long>(c.bytes_written), static_cast<unsigned long long>(c.guest_read_failures),
               static_cast<unsigned long long>(c.overflow), static_cast<unsigned long long>(g.vblank), c.largest_items,
               c.largest_draw_bytes, c.largest_frame_bytes, static_cast<unsigned long long>(g.pass_count),
               static_cast<unsigned long long>(g.orphan_pass_ends), static_cast<unsigned long long>(c.light_blocks),
               static_cast<unsigned long long>(c.light_blocks_missing),
               static_cast<unsigned long long>(c.program_selectors),
               static_cast<unsigned long long>(c.program_selectors_missing),
               static_cast<unsigned long long>(c.light_items_uploaded),
               static_cast<unsigned long long>(c.light_items_carried),
               static_cast<unsigned long long>(c.light_items_uncertain),
               static_cast<unsigned long long>(c.light_items_none),
               static_cast<unsigned long long>(c.program_items),
               static_cast<unsigned long long>(c.program_items_missing),
               static_cast<unsigned long long>(c.program_item_variants),
               static_cast<unsigned long long>(c.program_item_mscal_variants),
               static_cast<unsigned long long>(c.chain_calls),
               static_cast<unsigned long long>(c.chain_gaps),
               static_cast<unsigned long long>(c.chain_incomplete),
               static_cast<unsigned long long>(c.morph_uploads),
               static_cast<unsigned long long>(c.morph_draws),
               static_cast<unsigned long long>(c.morph_reads),
               static_cast<unsigned long long>(c.morph_mismatches),
               static_cast<unsigned long long>(c.morph_value_mismatches),
               static_cast<unsigned long long>(c.morph_missing));
  // The VU1 data memory is double buffered (VIF BASE/OFFSET). The EE-side shadow VIF1 unit (it parses every submitted
  // VIF1 chain in dry-run mode) reports the registers that define the two 0xC9 locations, so the capture states the
  // parity context of its carried block rather than assuming OFFSET is zero.
  const std::uint32_t vif_stat = ps2::vif::read(1, 0x00u);
  std::fprintf(out,
               "[coregs3] vif1 shadow stat 0x%08X dbf %u base %u ofst %u itops %u tops %u top %u runs %u gap_epoch %u\n",
               vif_stat, (vif_stat >> 7) & 1u, ps2::vif::read(1, 0xA0u), ps2::vif::read(1, 0xB0u),
               ps2::vif::read(1, 0x90u), ps2::vif::read(1, 0xC0u), ps2::vif::read(1, 0xE0u), g.carried_light.runs,
               g.carried_light.gap_epoch);
  std::fprintf(out,
               "[coregs3] pass journal %llu environment_frames %llu environment_read_failures %llu pass_orphan_ends "
               "%llu pass_mismatched_ends %llu\n",
               static_cast<unsigned long long>(c.pass_states),
               static_cast<unsigned long long>(c.environment_frames),
               static_cast<unsigned long long>(c.environment_read_failures),
               static_cast<unsigned long long>(c.pass_orphan_ends),
               static_cast<unsigned long long>(c.pass_mismatched_ends));
  for (std::uint32_t pass = 0; pass < kPassCount; ++pass)
    if (c.pass_draws[pass] != 0 || c.pass_items[pass] != 0)
      std::fprintf(out, "[coregs3] pass %s draws %llu items %llu\n", pass_name(pass),
                   static_cast<unsigned long long>(c.pass_draws[pass]),
                   static_cast<unsigned long long>(c.pass_items[pass]));
  // TASK-090 live world: the in-memory per-job texture store. Every field is atomic; the report thread reads them.
  const live_world::Counters &lw = live_world::counters();
  if (lw.sources.load(std::memory_order_relaxed) != 0 || lw.job_records.load(std::memory_order_relaxed) != 0)
    std::fprintf(out,
                 "[coregs3] live world store sources %llu dropped_sources %llu records %llu unlinked %llu refused "
                 "%llu pruned %llu bytes %llu lookups %llu missing %llu\n",
                 static_cast<unsigned long long>(lw.sources.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.sources_dropped.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.job_records.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.jobs_unlinked.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.jobs_refused.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.records_pruned.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.bytes_held.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.lookups.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lw.lookup_missing.load(std::memory_order_relaxed)));
  const TapCounters &tap = tap_counters();
  if (tap.records != 0)
    std::fprintf(out, "[coregs3] tap records %llu bytes %llu refused %llu\n",
                 static_cast<unsigned long long>(tap.records), static_cast<unsigned long long>(tap.bytes),
                 static_cast<unsigned long long>(tap.refused));
  // TASK-090 S1: the `.gt4gs` stream's counters and its per-pass census (the same shape as the scene pass journal's).
  gsstream_print_report(out);
  const GsStreamCounters &gs = gsstream_counters();
  if (gs.active)
    for (std::uint32_t pass = 0; pass < kPassCount; ++pass)
      if (gs.pass_gif_records[pass] != 0 || gs.pass_gif_bytes[pass] != 0)
        std::fprintf(out, "[coregs3] gsstream pass %s records %llu bytes %llu\n", pass_name(pass),
                     static_cast<unsigned long long>(gs.pass_gif_records[pass]),
                     static_cast<unsigned long long>(gs.pass_gif_bytes[pass]));
  // The live layer's own counters (docs/COREGS3-HOOKS.md §10.3): frames published to the ring and, in `present`, what
  // the renderer drew over the composed image. Read from the report thread; every field is atomic.
  const present::Counters &p = present::counters();
  const auto load = [](const std::atomic<std::uint64_t> &value) { return value.load(std::memory_order_relaxed); };
  // TASK-090 live timeline: the requested rate, the pairing/interpolation outcome and the frame-time histogram of the
  // presentation clock. `mean`/`min`/`max` are the interval between consecutive presentation instants, in guest ticks
  // (3600 per second), so 30 ticks = 120 Hz and 60 ticks = 60 Hz.
  const present::TimelineConfig &timeline = present::timeline_config();
  const present::TimelineCounters &t = present::timeline_counters();
  const std::uint64_t intervals = load(t.interval_count);
  if (timeline.hz != 0 && (load(t.frames) != 0 || load(t.instants) != 0))
    std::fprintf(out,
                 "[coregs3] present timeline hz=%u mode=%s frames=%llu instants=%llu skipped=%llu draw_ticks=%llu "
                 "intervals=%llu interval_ticks_mean=%.3f min=%llu max=%llu\n",
                 timeline.hz, presentation_mode_name(timeline.mode),
                 static_cast<unsigned long long>(load(t.frames)),
                 static_cast<unsigned long long>(load(t.instants)),
                 static_cast<unsigned long long>(load(t.skipped)),
                 static_cast<unsigned long long>(load(t.draw_ticks)),
                 static_cast<unsigned long long>(intervals),
                 intervals != 0 ? static_cast<double>(load(t.interval_sum)) / static_cast<double>(intervals) : 0.0,
                 static_cast<unsigned long long>(load(t.interval_min)),
                 static_cast<unsigned long long>(load(t.interval_max)));
  if (timeline.hz > 60)
    std::fprintf(out,
                 "[coregs3] present host clock plans=%llu ticks=%llu late=%llu stale=%llu skipped=%llu "
                 "duplicates=%llu\n",
                 static_cast<unsigned long long>(load(t.host_plans)),
                 static_cast<unsigned long long>(load(t.host_ticks)),
                 static_cast<unsigned long long>(load(t.host_late)),
                 static_cast<unsigned long long>(load(t.host_stale)),
                 static_cast<unsigned long long>(load(t.host_skipped)),
                 static_cast<unsigned long long>(load(t.host_duplicates)));
  if (timeline.hz != 0 &&
      load(t.pairs) + load(t.pairs_refused) + load(t.paired) + load(t.only_newer) + load(t.only_older) +
              load(t.interpolated) + load(t.entrywise) + load(t.nearest) + load(t.dropped_discontinuity) !=
          0)
    std::fprintf(out,
                 "[coregs3] present interpolate pairs=%llu pairs_refused=%llu paired=%llu only_newer=%llu "
                 "only_older=%llu interpolated=%llu entrywise=%llu nearest=%llu predicted=%llu "
                 "dropped_discontinuity=%llu\n",
                 static_cast<unsigned long long>(load(t.pairs)),
                 static_cast<unsigned long long>(load(t.pairs_refused)),
                 static_cast<unsigned long long>(load(t.paired)),
                 static_cast<unsigned long long>(load(t.only_newer)),
                 static_cast<unsigned long long>(load(t.only_older)),
                 static_cast<unsigned long long>(load(t.interpolated)),
                 static_cast<unsigned long long>(load(t.entrywise)),
                 static_cast<unsigned long long>(load(t.nearest)),
                 static_cast<unsigned long long>(load(t.predicted)),
                 static_cast<unsigned long long>(load(t.dropped_discontinuity)));
  if (timeline.hz != 0)
    for (std::size_t bucket = 0; bucket < t.interval_hist.size(); ++bucket)
      if (load(t.interval_hist[bucket]) != 0)
        std::fprintf(out, "[coregs3] present timeline interval %llu..%llu ticks: %llu\n",
                     static_cast<unsigned long long>(bucket * 8),
                     static_cast<unsigned long long>(bucket == t.interval_hist.size() - 1 ? 0 : bucket * 8 + 7),
                     static_cast<unsigned long long>(load(t.interval_hist[bucket])));
  if (load(p.published) != 0 || load(p.passes) != 0 || load(p.passes_refused) != 0)
    std::fprintf(out,
                 "[coregs3] present published %llu dropped %llu claims %llu empty %llu passes %llu refused_frames %llu "
                 "draws %llu refused_draws %llu unlit_draws %llu vertices %llu unlit_vertices %llu triangles %llu "
                 "last_vblank %llu\n",
                 static_cast<unsigned long long>(load(p.published)),
                 static_cast<unsigned long long>(load(p.dropped_publish)),
                 static_cast<unsigned long long>(load(p.claims)),
                 static_cast<unsigned long long>(load(p.empty_claims)),
                 static_cast<unsigned long long>(load(p.passes)),
                 static_cast<unsigned long long>(load(p.passes_refused)),
                 static_cast<unsigned long long>(load(p.draws)),
                 static_cast<unsigned long long>(load(p.draws_refused)),
                 static_cast<unsigned long long>(load(p.draws_unlit)),
                 static_cast<unsigned long long>(load(p.vertices)),
                 static_cast<unsigned long long>(load(p.vertices_unlit)),
                 static_cast<unsigned long long>(load(p.triangles)),
                 static_cast<unsigned long long>(load(p.last_vblank)));
  if (load(p.world_frames) != 0)
    std::fprintf(out,
                 "[coregs3] live world frames %llu items %llu vertices %llu triangles %llu draws %llu ticks %llu "
                 "draw_calls %llu samples %llu atlas_textures %llu atlas_refused %llu texture_live %llu "
                 "texture_stale %llu texture_missing %llu texture_refused %llu refused_draws %llu excluded_itop %llu "
                 "excluded_shape %llu excluded_no_state %llu excluded_mirror %llu excluded_no_position %llu "
                 "skipped_reflection %llu sky_skipped %llu far_skipped %llu fog_items %llu fog_missing %llu "
                 "blend_deferred %llu depth_defaulted %llu alpha_fail_defaulted %llu texture_untextured %llu "
                 "texture_no_record %llu texture_no_uv %llu texture_texgen %llu filter_linear %llu filter_defaulted %llu "
                 "mip_defaulted %llu post_items %llu post_faithful %llu post_env_missing %llu light_draws_lit %llu "
                 "light_draws_fallback %llu light_vertices_lit %llu light_vertices_fallback %llu\n",
                 static_cast<unsigned long long>(load(p.world_frames)),
                 static_cast<unsigned long long>(load(p.world_items)),
                 static_cast<unsigned long long>(load(p.world_vertices)),
                 static_cast<unsigned long long>(load(p.world_triangles)),
                 static_cast<unsigned long long>(load(p.world_draws)),
                 static_cast<unsigned long long>(load(p.world_ticks)),
                 static_cast<unsigned long long>(load(p.world_draw_calls)),
                 static_cast<unsigned long long>(load(p.world_samples)),
                 static_cast<unsigned long long>(load(p.world_atlas_textures)),
                 static_cast<unsigned long long>(load(p.world_atlas_refused)),
                 static_cast<unsigned long long>(load(p.world_texture_live)),
                 static_cast<unsigned long long>(load(p.world_texture_stale)),
                 static_cast<unsigned long long>(load(p.world_texture_missing)),
                 static_cast<unsigned long long>(load(p.world_texture_refused)),
                 static_cast<unsigned long long>(load(p.world_refused_draws)),
                 static_cast<unsigned long long>(load(p.world_excluded_itop)),
                 static_cast<unsigned long long>(load(p.world_excluded_shape)),
                 static_cast<unsigned long long>(load(p.world_excluded_no_state)),
                 static_cast<unsigned long long>(load(p.world_excluded_mirror)),
                 static_cast<unsigned long long>(load(p.world_excluded_no_position)),
                 static_cast<unsigned long long>(load(p.world_skipped_reflection)),
                 static_cast<unsigned long long>(load(p.world_sky_skipped)),
                 static_cast<unsigned long long>(load(p.world_far_skipped)),
                 static_cast<unsigned long long>(load(p.world_fog_items)),
                 static_cast<unsigned long long>(load(p.world_fog_missing)),
                 static_cast<unsigned long long>(load(p.world_blend_deferred)),
                 static_cast<unsigned long long>(load(p.world_depth_defaulted)),
                 static_cast<unsigned long long>(load(p.world_alpha_fail_defaulted)),
                 static_cast<unsigned long long>(load(p.world_texture_untextured)),
                 static_cast<unsigned long long>(load(p.world_texture_no_record)),
                 static_cast<unsigned long long>(load(p.world_texture_no_uv)),
                 static_cast<unsigned long long>(load(p.world_texture_texgen)),
                 static_cast<unsigned long long>(load(p.world_filter_linear)),
                 static_cast<unsigned long long>(load(p.world_filter_defaulted)),
                 static_cast<unsigned long long>(load(p.world_mip_defaulted)),
                 static_cast<unsigned long long>(load(p.world_post_items)),
                 static_cast<unsigned long long>(load(p.world_post_faithful)),
                 static_cast<unsigned long long>(load(p.world_post_env_missing)),
                 static_cast<unsigned long long>(load(p.world_light_draws_lit)),
                 static_cast<unsigned long long>(load(p.world_light_draws_fallback)),
                 static_cast<unsigned long long>(load(p.world_light_vertices_lit)),
                 static_cast<unsigned long long>(load(p.world_light_vertices_fallback)));
  // Per-family and per-status lighting detail of the live layer (coregs3_lighting.hpp programs). Cumulative since the
  // process start; a draw may appear in more than one family because the capture mixes programs inside a draw.
  for (std::uint32_t program = 0; program < kLightProgramCount; ++program) {
    const std::uint64_t items = load(p.family_items[program]);
    if (items == 0) continue;
    std::fprintf(out,
                 "[coregs3] present family %s items %llu lit %llu fallback %llu vertices_lit %llu "
                 "vertices_fallback %llu draws_lit %llu draws_fallback %llu\n",
                 light_program_name(static_cast<LightProgram>(program)), static_cast<unsigned long long>(items),
                 static_cast<unsigned long long>(load(p.family_items_lit[program])),
                 static_cast<unsigned long long>(load(p.family_items_fallback[program])),
                 static_cast<unsigned long long>(load(p.family_vertices_lit[program])),
                 static_cast<unsigned long long>(load(p.family_vertices_fallback[program])),
                 static_cast<unsigned long long>(load(p.family_draws_lit[program])),
                 static_cast<unsigned long long>(load(p.family_draws_fallback[program])));
  }
  for (std::uint32_t status = 0; status < kLightStatusCount; ++status)
    if (load(p.light_status[status]) != 0)
      std::fprintf(out, "[coregs3] present status %s vertices %llu\n",
                   light_status_name(static_cast<LightStatus>(status)),
                   static_cast<unsigned long long>(load(p.light_status[status])));
  if (load(p.items_uploaded) != 0 || load(p.items_carried) != 0 || load(p.items_uncertain) != 0 ||
      load(p.items_none) != 0)
    std::fprintf(out, "[coregs3] present light_sources uploaded %llu carried %llu uncertain %llu none %llu\n",
                 static_cast<unsigned long long>(load(p.items_uploaded)),
                 static_cast<unsigned long long>(load(p.items_carried)),
                 static_cast<unsigned long long>(load(p.items_uncertain)),
                 static_cast<unsigned long long>(load(p.items_none)));
}

}  // namespace gt4::coregs3
