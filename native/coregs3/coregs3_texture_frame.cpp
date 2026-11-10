// TASK-090 CoreGS 3.0: the per-frame texture table. See coregs3_texture_frame.hpp.
#include "coregs3_texture_frame.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "coregs3_gif.hpp"
#include "coregs3_lighting.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_tap.hpp"
#include "coregs3_vram.hpp"

namespace gt4::coregs3 {
namespace {

// The tracked GS state of the tap walk. `SampleState` carries the per-field "known" flags; TEX0 is kept separately
// because it is the pairing key, not a pixel state.
struct TrackedState {
  SampleState state;
  std::uint32_t tbp0 = 0;
  std::uint32_t psm = 0;
  bool tex0_known = false;
};

// One tapped vertex record of a job that also carries the frame's draws.
struct TapStream {
  std::uint32_t vertices = 0;
  bool tex0_known = false;
  std::uint32_t tbp0 = 0;
  std::uint32_t psm = 0;
  SampleState state;
  // The per-vertex fog values of the tapped record (0..255), one per vertex, in emission order. Empty when no vertex
  // of the record carried a fog value; the reflection pass then draws with fog 255 (no fog) and counts the fallback.
  std::vector<std::uint8_t> fog;
};

std::uint32_t bits(std::uint64_t value, unsigned low, unsigned high) {
  const std::uint64_t mask = high - low == 63 ? ~0ull : ((1ull << (high - low + 1)) - 1ull);
  return static_cast<std::uint32_t>((value >> low) & mask);
}

void apply_register(TrackedState &tracked, const GifRegisterWrite &write, TextureFrameCounters &counters) {
  counters.tap_state_writes += 1;
  switch (write.address) {
    case 0x06u:  // TEX0_1
    case 0x07u:  // TEX0_2
      tracked.tbp0 = bits(write.data, 0, 13);
      tracked.psm = bits(write.data, 20, 25);
      tracked.tex0_known = true;
      break;
    case 0x42u:  // ALPHA_1
    case 0x43u:  // ALPHA_2
      tracked.state.alpha = write.data;
      tracked.state.alpha_known = true;
      break;
    case 0x47u:  // TEST_1
    case 0x48u:  // TEST_2
      tracked.state.test = write.data;
      tracked.state.test_known = true;
      break;
    case 0x4Cu:  // FRAME_1
    case 0x4Du:  // FRAME_2
      tracked.state.fbmsk = frame_fbmsk(write.data);
      tracked.state.frame_known = true;
      break;
    case 0x4Eu:  // ZBUF_1
    case 0x4Fu:  // ZBUF_2
      tracked.state.zmsk = zbuf_zmsk(write.data);
      tracked.state.zbuf_known = true;
      break;
    case 0x3Du:  // FOGCOL
      tracked.state.fog_colour = static_cast<std::uint32_t>(write.data & 0xFFFFFFu);
      break;
    default: break;
  }
}

// One decoded record, reused by the items that name it.
struct RecordTexture {
  bool decoded = false;
  const SampledTexture *sampler = nullptr;  // the record's own sampling state (shared by its items)
};

}  // namespace

bool build_frame_texture_table(const FrameSnapshot &frame, const SlmImage &slm, const std::uint8_t *tap_data,
                               const std::vector<TapFileRecord> &tap_records, const TextureFrameOptions &options,
                               FrameTextureTable &table, TextureFrameCounters &counters, std::string &error) {
  (void)error;
  table = FrameTextureTable{};
  counters = TextureFrameCounters{};
  table.draws.resize(frame.draws.size());
  table.sequence.resize(frame.draws.size());
  for (std::size_t index = 0; index < frame.draws.size(); ++index) table.sequence[index] = frame.draws[index].sequence;

  // ---- 1. The tapped vertex records of the jobs that posted this frame's item packets, with the state they ran under.
  std::vector<TapStream> streams;
  std::set<std::uint64_t> frame_jobs;
  if (options.use_tap && tap_data != nullptr) {
    std::map<std::uint32_t, std::vector<std::uint64_t>> address_jobs;
    for (const TapFileRecord &record : tap_records)
      if (record.kind == kTapKindSource && record.vblank == frame.vblank)
        address_jobs[record.address].push_back(record.job);
    for (const CapturedDraw &draw : frame.draws)
      for (std::uint32_t index = 0; index < draw.item_count; ++index) {
        const auto found = address_jobs.find(draw.items[index].address);
        if (found == address_jobs.end()) continue;
        for (std::uint64_t job : found->second) frame_jobs.insert(job);
      }
    if (!frame_jobs.empty()) {
      TrackedState tracked;
      tracked.state = measured_sample_state();
      for (const TapFileRecord &record : tap_records) {
        if (record.kind != kTapKindGif) continue;
        counters.tap_records += 1;
        const GifPacket packet = parse_gif(tap_data + record.offset, record.bytes);
        if (!packet.ok) continue;
        const bool in_frame_jobs = frame_jobs.count(record.job) != 0;
        if (in_frame_jobs && !packet.vertices.empty()) {
          TapStream stream;
          stream.vertices = static_cast<std::uint32_t>(packet.vertices.size());
          stream.tex0_known = tracked.tex0_known;
          stream.tbp0 = tracked.tbp0;
          stream.psm = tracked.psm;
          stream.state = tracked.state;
          stream.state.from_tap = true;
          // The record's per-vertex fog (the GS fog lane of XYZF2). A record whose vertices carry no fog keeps an empty
          // vector so the renderer can count the fallback instead of inventing values.
          bool any_fog = false;
          stream.fog.reserve(packet.vertices.size());
          for (const GifVertex &vertex : packet.vertices) {
            if (vertex.has_fog) any_fog = true;
            stream.fog.push_back(vertex.has_fog ? vertex.fog : 255u);
          }
          if (!any_fog) {
            stream.fog.clear();
            counters.reflection_fog_missing += 1;
          }
          if (!stream.state.prim_known) {
            // PRIM is a GIFtag field as well as a register write: the GS applies the tag's PRIM for the transfer, and
            // the car pass sets PRIM only through the tag (no PRIM register write appears in the captured stream).
            stream.state.prim = !packet.tags.empty() ? packet.tags.front().prim : packet.vertices.front().prim;
            stream.state.prim_known = true;
          }
          streams.push_back(stream);
        }
        // A record's A+D writes apply after its vertices: the measured streams put state in vertex-less records, and
        // the `uv` gate's proven rule ignores a state write that shares a vertex record.
        for (const GifRegisterWrite &write : packet.ad_writes) apply_register(tracked, write, counters);
      }
    }
  }

  // ---- 2. Per-item channel counts and the model UV flag (the same channel rule build_mesh applies).
  std::vector<std::vector<std::uint32_t>> counts(frame.draws.size());
  std::vector<std::vector<bool>> has_uv(frame.draws.size());
  for (std::size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    counts[draw_index].assign(draw.item_count, 0u);
    has_uv[draw_index].assign(draw.item_count, false);
    const ChannelScan scan = scan_channels(frame.packets.data(), frame.packets.size(), draw);
    for (const ShapeChannel &channel : scan.channels) {
      if (channel.item >= draw.item_count) continue;
      if (channel.role == ChannelRole::positions) counts[draw_index][channel.item] += channel.vectors;
      if (channel.role == ChannelRole::uv) has_uv[draw_index][channel.item] = true;
    }
  }

  // ---- 3. The Format G key cache (decode once per key per frame) and the per-record sampling states.
  std::vector<std::pair<TextureKey, std::uint32_t>> cache;  // key -> payload slot
  std::vector<RecordTexture> record_texture(frame.textures.size());

  const auto decode_record = [&](std::uint32_t record_index,
                                 const CapturedTexture &record) -> const SampledTexture * {
    RecordTexture &entry = record_texture[record_index];
    if (entry.decoded) return entry.sampler;
    entry.decoded = true;
    DecodedTexture decoded;
    std::string decode_error;
    if (decode_texture(slm, record.bytes, sizeof record.bytes, decoded, decode_error) != TextureStatus::ok) {
      counters.records_refused += 1;
      return nullptr;
    }
    counters.records_decoded += 1;
    const TextureKey key{decoded.key.textureHash, decoded.key.paletteHash, decoded.key.width, decoded.key.height};
    std::uint32_t slot = 0;
    bool cached = false;
    for (const auto &[cached_key, cached_slot] : cache) {
      if (!(cached_key == key)) continue;
      slot = cached_slot;
      cached = true;
      break;
    }
    if (cached) {
      counters.cache_hits += 1;
    } else {
      slot = static_cast<std::uint32_t>(table.payloads.size());
      table.payloads.push_back(decoded.rgba);
      cache.emplace_back(key, slot);
    }
    SampledTexture sampler;
    sampler.psm = decoded.record.psm;
    sampler.tw = decoded.record.tw;
    sampler.th = decoded.record.th;
    sampler.tcc = decoded.record.tcc;
    sampler.tfx = decoded.record.tfx;
    sampler.mxl = decoded.record.mxl;
    sampler.mmag = decoded.record.mmag;
    sampler.mmin = decoded.record.mmin;
    sampler.wms = decoded.record.wms;
    sampler.wmt = decoded.record.wmt;
    sampler.minu = decoded.record.minu;
    sampler.maxu = decoded.record.maxu;
    sampler.minv = decoded.record.minv;
    sampler.maxv = decoded.record.maxv;
    sampler.width = decoded.width;
    sampler.height = decoded.height;
    sampler.x0 = decoded.x0;
    sampler.y0 = decoded.y0;
    sampler.cache_slot = slot;
    sampler.rgba = table.payloads[slot].data();
    table.textures.push_back(sampler);
    entry.sampler = &table.textures.back();
    return entry.sampler;
  };

  // ---- 4. Bind every item and attach its tapped state.
  for (std::size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    std::vector<TextureBinding> &bindings = table.draws[draw_index];
    bindings.resize(draw.item_count);
    std::size_t cursor = 0;
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      TextureBinding &binding = bindings[index];
      binding.state = measured_sample_state();
      binding.texture_record = item.texture_record;
      counters.items_visited += 1;

      // The item's tapped state, when the tap paired it. The pairing advances across the item list in order.
      std::size_t chosen = 0;
      bool matched = false;
      if (!streams.empty() && item.packet_bytes != 0 && counts[draw_index][index] != 0) {
        const bool has_record = item.texture_record != kNoRecord && item.texture_record < frame.textures.size();
        std::uint32_t item_tbp0 = 0;
        std::uint32_t item_psm = 0;
        if (has_record) {
          const std::uint64_t tex0 = vram::read_u64(frame.textures[item.texture_record].bytes, 0);
          item_tbp0 = bits(tex0, 0, 13);
          item_psm = bits(tex0, 20, 25);
        }
        std::uint32_t candidates = 0;
        for (std::size_t stream = cursor; stream < streams.size(); ++stream) {
          if (streams[stream].vertices != counts[draw_index][index]) continue;
          if (has_record &&
              (!streams[stream].tex0_known || streams[stream].tbp0 != item_tbp0 ||
               streams[stream].psm != item_psm))
            continue;
          if (!matched) {
            chosen = stream;
            matched = true;
          }
          ++candidates;
        }
        if (matched) {
          binding.state = streams[chosen].state;
          cursor = chosen + 1;
          counters.tap_items_paired += 1;
          if (candidates > 1) counters.tap_items_ambiguous += 1;
        } else {
          counters.tap_items_defaulted += 1;
        }
      } else if (!streams.empty()) {
        counters.tap_items_defaulted += 1;
      }

      // The texture, if the item samples one.
      if (item.texture_record == kNoRecord || item.texture_record >= frame.textures.size()) {
        counters.items_no_texture += 1;
        counters.items_untextured += 1;
        continue;
      }
      const LightProgram program = light_program_of_item(item);
      if (program == LightProgram::body_paint && options.reflections) {
        // The reflection milestone: the item's second pass samples its CT32 environment map with the pinned texgen S,T
        // (computed by the renderer from the item's block and the mesh vertex). The TEX1/CLAMP state is the item's own
        // 40-byte record (linear/linear, region clamp); TEST/ALPHA/FRAME/ZBUF/FOGCOL and the per-vertex fog are the
        // tapped PRIM 0x7C record. Without a usable tapped record the item keeps the lit colour and is counted.
        const bool usable_state = matched && streams[chosen].vertices == counts[draw_index][index];
        if (!usable_state) {
          counters.items_reflection_no_state += 1;
          counters.items_untextured += 1;
          continue;
        }
        const SampledTexture *sampler = decode_record(item.texture_record, frame.textures[item.texture_record]);
        if (sampler == nullptr) {
          counters.items_decode_refused += 1;
          counters.items_untextured += 1;
          continue;
        }
        binding.texture = sampler;
        binding.reflection = true;
        binding.vertex_fog = streams[chosen].fog;
        counters.items_reflected += 1;
        continue;
      }
      if (program == LightProgram::body_paint) {
        // Pre-reflection mode: the body/paint family samples its environment map with VU1 texgen ST, not the model UV.
        // It keeps the lit colour and records the deferral instead of sampling the wrong coordinate.
        counters.items_texgen_deferred += 1;
        counters.items_untextured += 1;
        continue;
      }
      if (program == LightProgram::none) counters.items_program_unknown += 1;
      if (!has_uv[draw_index][index]) {
        counters.items_no_uv += 1;
        counters.items_untextured += 1;
        continue;
      }
      const SampledTexture *sampler = decode_record(item.texture_record, frame.textures[item.texture_record]);
      if (sampler == nullptr) {
        counters.items_decode_refused += 1;
        counters.items_untextured += 1;
        continue;
      }
      binding.texture = sampler;
      counters.items_textured += 1;
    }
  }
  return true;
}

}  // namespace gt4::coregs3
