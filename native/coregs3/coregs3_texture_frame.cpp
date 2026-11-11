// TASK-090 CoreGS 3.0: the per-frame texture table. See coregs3_texture_frame.hpp.
#include "coregs3_texture_frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "coregs3_gif.hpp"
#include "coregs3_lighting.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_tap.hpp"
#include "coregs3_vram.hpp"
#include "hw_gs_lod.hpp"
#include "hw_gs_registers.hpp"

namespace gt4::coregs3 {
namespace {

// The tracked GS state of the tap walk. `SampleState` carries the per-field "known" flags; TEX0 is kept separately
// because it is the pairing key, not a pixel state.
struct TrackedState {
  SampleState state;
  // TASK-090 track textures: the actual texture registers the VU1 emitted, for the world passes' state binding.
  TextureState texture;
};

// One tapped vertex record of a job that also carries the frame's draws.
struct TapStream {
  std::uint32_t vertices = 0;
  bool tex0_known = false;  // the car pairing key: the last TEX0 write's TBP0/PSM
  std::uint32_t tbp0 = 0;
  std::uint32_t psm = 0;
  SampleState state;
  // TASK-090 track textures: the full TEX0/TEX1/CLAMP/TEX2 state in force at the record's first vertex.
  TextureState texture;
  // The per-vertex fog values of the tapped record (0..255), one per vertex, in emission order. Empty when no vertex
  // of the record carried a fog value; the reflection pass then draws with fog 255 (no fog) and counts the fallback.
  std::vector<std::uint8_t> fog;
  // TASK-090 track M1: the tapped S/Q,T/Q of each vertex (the model UV the VU1 emitted), used to pair the item with the
  // exact record by the measured UV equality. `valid` is 0 for a vertex the record emitted without STQ.
  struct Stq {
    float u = 0.0f;
    float v = 0.0f;
    float q = 1.0f;  // the tapped STQ Q (TASK-090 texture mip: the explicit LOD formula's input)
    std::uint8_t valid = 0;
  };
  std::vector<Stq> stq;
};

std::uint32_t bits(std::uint64_t value, unsigned low, unsigned high) {
  const std::uint64_t mask = high - low == 63 ? ~0ull : ((1ull << (high - low + 1)) - 1ull);
  return static_cast<std::uint32_t>((value >> low) & mask);
}

void apply_register(TrackedState &tracked, const GifRegisterWrite &write, TextureFrameCounters *counters) {
  if (counters != nullptr) counters->tap_state_writes += 1;
  switch (write.address) {
    case 0x06u:  // TEX0_1
    case 0x07u:  // TEX0_2
      tracked.texture.tex0 = write.data;
      tracked.texture.tex0_known = true;
      break;
    case 0x08u:  // CLAMP_1
    case 0x09u:  // CLAMP_2
      tracked.texture.clamp = write.data;
      tracked.texture.clamp_known = true;
      break;
    case 0x14u:  // TEX1_1
    case 0x15u:  // TEX1_2
      tracked.texture.tex1 = write.data;
      tracked.texture.tex1_known = true;
      break;
    case 0x16u:  // TEX2_1
    case 0x17u:  // TEX2_2
      tracked.texture.tex2 = write.data;
      tracked.texture.tex2_known = true;
      break;
    case 0x34u:  // MIPTBP1_1
    case 0x35u:  // MIPTBP1_2
      tracked.texture.miptbp1 = write.data;
      tracked.texture.miptbp1_known = true;
      break;
    case 0x36u:  // MIPTBP2_1
    case 0x37u:  // MIPTBP2_2
      tracked.texture.miptbp2 = write.data;
      tracked.texture.miptbp2_known = true;
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
    case 0x18u:  // XYOFFSET_1
    case 0x19u:  // XYOFFSET_2
      tracked.state.xyoffset = write.data;
      tracked.state.xyoffset_known = true;
      break;
    default: break;
  }
}

// One decoded record, reused by the items that name it.
struct RecordTexture {
  bool decoded = false;
  const SampledTexture *sampler = nullptr;  // the record's own sampling state (shared by its items)
};

// TASK-090 track M1: the course pass's journal state, for an item whose exact tapped vertex record could not be paired.
// The K7 marker captures the PGL GS shadow at the pass begin; TEST/ALPHA/XYOFFSET/FOGCOL are the measured pass state
// (the journal's TEST/XYOFFSET match the tap wherever the pass's first draw does not rewrite them). The PRIM is not in
// the journal, so the measured default (0x3C, the majority course tag) is kept and the item is counted as pass-defaulted.
SampleState world_pass_default_state(const FrameSnapshot &frame, std::uint32_t pass) {
  SampleState state = measured_sample_state();
  for (const CapturedPassState &entry : frame.pass_states) {
    if (entry.pass != pass) continue;
    state.from_tap = true;
    state.world_pass_default = true;
    state.test = entry.test & 0xFFFFFFFFu;
    state.test_known = true;
    state.alpha = entry.alpha & 0xFFFFFFFFu;
    state.alpha_known = true;
    state.xyoffset = entry.xyoffset;
    state.xyoffset_known = true;
    state.fog_colour = static_cast<std::uint32_t>(entry.fogcol & 0xFFFFFFu);
    return state;
  }
  return state;
}

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

  // TASK-090 pass textures: the SLM image each pass begin was captured with. Slot 0 is the caller's default image (the
  // frame-end record); every supplied pass image gets its own slot, so a record or a tapped state decoded against one
  // image is never reused for another pass (the payload cache below is keyed by the Format G content hash as well).
  std::vector<const SlmImage *> image_slms{&slm};
  std::uint32_t pass_slot[kPassCount] = {};
  if (options.pass_slms != nullptr) {
    for (std::uint32_t pass = 0; pass < kPassCount; ++pass) {
      if (options.pass_slms->images[pass] == nullptr) continue;
      pass_slot[pass] = static_cast<std::uint32_t>(image_slms.size());
      image_slms.push_back(options.pass_slms->images[pass]);
      counters.world_pass_slm_images += 1;
    }
  }
  const auto is_world_pass = [](std::uint32_t pass) {
    // TASK-090 wet reflection: the reflection pass's 3D items bind their own VU1 job's pixels exactly like the world
    // passes (the pass samples the wet-road mask and the reflection quads from VRAM pages 0x3F28/0x3F80).
    return pass == kPassSky || pass == kPassFarUnderlay || pass == kPassCourse || pass == kPassReflection;
  };

  // ---- 1. The tapped vertex records of the jobs that posted this frame's item packets, with the state they ran under.
  std::vector<TapStream> streams;
  std::set<std::uint64_t> frame_jobs;
  std::map<std::uint32_t, std::vector<std::uint64_t>> address_jobs;
  std::map<std::uint32_t, std::uint32_t> job_of_address;
  if (options.use_tap && tap_data != nullptr) {
    for (const TapFileRecord &record : tap_records)
      if (record.kind == kTapKindSource && record.vblank == frame.vblank)
        address_jobs[record.address].push_back(record.job);
    for (const CapturedDraw &draw : frame.draws)
      for (std::uint32_t index = 0; index < draw.item_count; ++index) {
        const auto found = address_jobs.find(draw.items[index].address);
        if (found == address_jobs.end()) continue;
        for (std::uint64_t job : found->second) frame_jobs.insert(job);
      }
      // TASK-090 pass textures: the job each item's packet address was posted as (the tap's source record). The first
      // source record is the job the packet belongs to; the per-job SLM record is keyed by its low 32 bits.
      for (const auto &[address, jobs] : address_jobs)
        if (!jobs.empty()) job_of_address[address] = static_cast<std::uint32_t>(jobs.front() & 0xFFFFFFFFull);
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
          // The state the record runs under is the tracked state after the A+D writes that precede its first vertex in
          // transfer order (a write after the vertices applies to the next draw). This is the same per-item state the
          // item-state study measured; the car pass's vertex-less state records are unaffected (the prefix is empty).
          TrackedState at_first = tracked;
          for (std::uint32_t index = 0;
               index < packet.ad_writes_at_first_vertex && index < packet.ad_writes.size(); ++index)
            apply_register(at_first, packet.ad_writes[index], nullptr);
          TapStream stream;
          stream.vertices = static_cast<std::uint32_t>(packet.vertices.size());
          stream.tex0_known = at_first.texture.tex0_known;
          stream.tbp0 = hwgs::tex0::Tbp0(at_first.texture.tex0);
          stream.psm = hwgs::tex0::Psm(at_first.texture.tex0);
          stream.state = at_first.state;
          stream.state.from_tap = true;
          stream.texture = at_first.texture;
          // The record's per-vertex fog (the GS fog lane of XYZF2). A record whose vertices carry no fog keeps an empty
          // vector so the renderer can count the fallback instead of inventing values.
          bool any_fog = false;
          stream.fog.reserve(packet.vertices.size());
          stream.stq.reserve(packet.vertices.size());
          for (const GifVertex &vertex : packet.vertices) {
            if (vertex.has_fog) any_fog = true;
            stream.fog.push_back(vertex.has_fog ? vertex.fog : 255u);
            TapStream::Stq sample;
            if (vertex.has_st && vertex.q != 0.0f) {
              sample.u = vertex.s / vertex.q;
              sample.v = vertex.t / vertex.q;
              sample.q = vertex.q;
              sample.valid = 1;
            }
            stream.stq.push_back(sample);
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
        // Every A+D write applies to the tracked state for the records that follow; the snapshot above already applied
        // the prefix that precedes this record's first vertex, so the order is the GS's transfer order.
        for (const GifRegisterWrite &write : packet.ad_writes) apply_register(tracked, write, &counters);
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
    const ChannelScan scan = scan_channels(frame.packets.data(), frame.packets.size(), draw, options.world_channels);
    for (const ShapeChannel &channel : scan.channels) {
      if (channel.item >= draw.item_count) continue;
      if (!options.world_channels && channel.role == ChannelRole::positions)
        counts[draw_index][channel.item] += channel.vectors;
      if (!options.world_channels && channel.role == ChannelRole::uv) has_uv[draw_index][channel.item] = true;
    }
    if (options.world_channels) {
      // The world path pairs the tap by the item's descriptor vertex count (the true list length) and accepts the
      // V2-8/continuation UV arrays the scan reports.
      for (std::uint32_t item = 0; item < draw.item_count; ++item) {
        if (item >= scan.items.size()) continue;
        counts[draw_index][item] = scan.items[item].descriptor_vertices;
        has_uv[draw_index][item] = scan.items[item].has_uv;
      }
    }
  }

  // ---- 3. The Format G key cache (decode once per key per frame), the per-record sampling states and, for the world
  // passes, the per-tapped-state sampling states.
  std::vector<std::pair<TextureKey, std::uint32_t>> cache;  // key -> payload slot
  // TASK-090 pass textures: the record decode cache is keyed by (image id, record index). The image id is the pass
  // image's slot, or 0x40000000 | draw sequence for a per-draw overlay, whose content differs from every other image.
  std::map<std::uint64_t, RecordTexture> record_texture;
  // One entry per distinct tapped state the world passes bind: the (TEX0, TEX1, CLAMP) sampling words plus the last
  // TEX2 word when it is known, plus the SLM image slot the pixels were decoded from (two passes can bind the same
  // register words against different captured images). Two items with the same writes share one decoded sampler exactly
  // as two records with the same Format G key share one payload.
  struct StateKey {
    std::uint64_t tex0 = 0;
    std::uint64_t tex1 = 0;
    std::uint64_t clamp = 0;
    std::uint64_t tex2 = 0;
    std::uint32_t image = 0;
    std::uint64_t miptbp1 = 0;  // TASK-090 texture mip: the resolved chain words, so two images/states never share one
    std::uint64_t miptbp2 = 0;
  };
  const auto same_state = [](const StateKey &a, const StateKey &b) {
    return a.tex0 == b.tex0 && a.tex1 == b.tex1 && a.clamp == b.clamp && a.tex2 == b.tex2 && a.image == b.image &&
           a.miptbp1 == b.miptbp1 && a.miptbp2 == b.miptbp2;
  };
  std::vector<std::pair<StateKey, const SampledTexture *>> state_cache;

  // Finds or creates the payload slot of a decoded texture (the Format G content key cache).
  const auto payload_slot = [&](const DecodedTexture &decoded) -> std::uint32_t {
    const TextureKey key{decoded.key.textureHash, decoded.key.paletteHash, decoded.key.width, decoded.key.height};
    for (const auto &[cached_key, cached_slot] : cache) {
      if (cached_key == key) {
        counters.cache_hits += 1;
        return cached_slot;
      }
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(table.payloads.size());
    table.payloads.push_back(decoded.rgba);
    cache.emplace_back(key, slot);
    return slot;
  };

  // Builds the sampler the renderer reads from a decoded texture. The level-0 mip and filter fallbacks are the world
  // path's declared substitution; the car path keeps its measured refusal.
  const auto make_sampler = [&](const DecodedTexture &decoded, std::uint32_t slot, bool from_state,
                                const TextureState *state) {
    SampledTexture sampler;
    sampler.psm = decoded.record.psm;
    sampler.tw = decoded.record.tw;
    sampler.th = decoded.record.th;
    sampler.tcc = decoded.record.tcc;
    sampler.tfx = decoded.record.tfx;
    // TASK-090 texture mip: a complete chain keeps the record's MXL and real MMAG/MMIN; without one the sampler keeps
    // the level-0 fallback (MXL stored as 0 so the level-0 subset accepts it) and the fact is counted.
    sampler.mip_ready = !decoded.mip_levels.empty();
    sampler.mxl = sampler.mip_ready ? decoded.record.mxl : 0u;
    sampler.miptbp1 = decoded.miptbp1;
    sampler.miptbp2 = decoded.miptbp2;
    sampler.miptbp_from_state = decoded.miptbp_from_state;
    sampler.mip_levels.resize(decoded.mip_levels.size());
    for (std::size_t level = 0; level < decoded.mip_levels.size(); ++level) {
      const DecodedTexture::MipLevel &source = decoded.mip_levels[level];
      SampledTexture::MipLevel &destination = sampler.mip_levels[level];
      destination.level = source.level;
      destination.width = source.width;
      destination.height = source.height;
      destination.rgba = source.rgba;
    }
    sampler.level0_mip_fallback = decoded.level0_mip_fallback;
    sampler.lcm = decoded.record.lcm;
    sampler.lod_l = 0;
    sampler.lod_k_raw = 0;
    sampler.mmag = decoded.record.mmag;
    sampler.mmin = decoded.record.mmin;
    if (decoded.level0_mip_fallback) {
      // The declared level-0 fallback: the record asks for mips (MMIN 2, mip nearest) but only level 0 is decoded, so the
      // GS's minification filter at level 0 is its low bit (0 = nearest) and the magnification filter is replaced by the
      // same nearest form. The mixed MMAG/MMIN pair the car sampler refuses is therefore not passed on.
      sampler.mmag = sampler.mmin = decoded.record.mmin & 1u;
    } else if (!sampler.mip_ready && options.world_channels && decoded.record.mmin > 1u &&
               decoded.record.mmag != decoded.record.mmin) {
      // The world path's level-0 filter fallback: the record asks for a mip filter (MMIN 2/3) but only level 0 is
      // decoded here, so the GS's level-0 minification filter is MMIN's low bit (2 -> nearest, 3 -> linear) and the
      // magnification filter is set to the same form. Counted; the car path keeps its measured refusal.
      sampler.mmag = sampler.mmin = decoded.record.mmin & 1u;
      counters.records_filter_fallback += 1;
    }
    if (sampler.mip_ready) {
      const hwgs::lod::Tex1Fields fields = hwgs::lod::decode_tex1(decoded.record.tex1);
      sampler.lcm = fields.lcm ? 1u : 0u;
      sampler.lod_l = fields.l;
      sampler.lod_k_raw = fields.k_raw;
    }
    sampler.tbp0 = decoded.record.tbp0;
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
    sampler.from_state = from_state;
    sampler.palette_via_tex2 = decoded.palette_via_tex2;
    // The register words the sampler state was decoded from: the tapped writes on the state path, the record's own
    // words on the fallback. The binding table prints them either way.
    sampler.source_tex0 = state != nullptr ? state->tex0 : decoded.record.tex0;
    sampler.source_tex1 = state != nullptr ? state->tex1 : decoded.record.tex1;
    sampler.source_clamp = state != nullptr ? state->clamp : decoded.record.clamp;
    return sampler;
  };

  const auto decode_record = [&](std::uint32_t record_index, const CapturedTexture &record, const SlmImage &image,
                                 std::uint32_t image_id) -> const SampledTexture * {
    const std::uint64_t key = (static_cast<std::uint64_t>(image_id) << 32) | record_index;
    RecordTexture &entry = record_texture[key];
    if (entry.decoded) return entry.sampler;
    entry.decoded = true;
    DecodedTexture decoded;
    std::string decode_error;
    TextureDecodeOptions decode_options;
    decode_options.level0_mip_fallback = options.world_channels;
    decode_options.decode_mip_levels = options.world_channels && options.world_mip;
    if (decode_texture(image, record.bytes, sizeof record.bytes, decode_options, decoded, decode_error) !=
        TextureStatus::ok) {
      counters.records_refused += 1;
      return nullptr;
    }
    counters.records_decoded += 1;
    if (decoded.level0_mip_fallback) counters.records_level0_fallback += 1;
    if (decoded.mip_levels_missing != 0u) counters.records_mip_levels_missing += 1;
    if (!decoded.mip_levels.empty()) counters.records_mip_chains += 1;
    const std::uint32_t slot = payload_slot(decoded);
    table.textures.push_back(make_sampler(decoded, slot, false, nullptr));
    entry.sampler = &table.textures.back();
    return entry.sampler;
  };

  // TASK-090 track textures: decode from the tapped GS state. The payload is shared with the record path by Format G
  // key; the sampler itself is per distinct state, because the filter/wrap fields come from the registers, not the
  // record. A refusal is counted and named, never substituted with the record.
  const auto decode_state = [&](const TextureState &state, const SlmImage &image, std::uint32_t image_id,
                                const CapturedTexture *record) -> const SampledTexture * {
    // TASK-090 texture mip: the MIPTBP words. The tap wins when the VU1 stream wrote them; otherwise the item's 40-byte
    // record supplies them (the texture set's own words), counted as inherited. No source means the chain cannot be
    // proven and the level-0 fallback applies.
    std::uint64_t miptbp1 = 0, miptbp2 = 0;
    bool mip_registers_known = false;
    if (state.miptbp1_known || state.miptbp2_known) {
      miptbp1 = state.miptbp1;
      miptbp2 = state.miptbp2;
      mip_registers_known = true;
    } else if (record != nullptr) {
      TextureRecord fields;
      std::string parse_error;
      if (parse_texture_record(record->bytes, sizeof record->bytes, fields, parse_error)) {
        miptbp1 = fields.miptbp1;
        miptbp2 = fields.miptbp2;
        mip_registers_known = true;
        counters.world_mip_registers_from_record += 1;
      }
    }
    if (state.miptbp1_known || state.miptbp2_known) counters.world_mip_registers_from_tap += 1;
    const StateKey key{state.tex0, state.tex1, state.clamp, state.tex2_known ? state.tex2 : 0u, image_id,
                       miptbp1, miptbp2};
    for (const auto &[cached_key, cached_sampler] : state_cache)
      if (same_state(cached_key, key)) return cached_sampler;
    DecodedTexture decoded;
    std::string decode_error;
    TextureDecodeOptions decode_options;
    decode_options.level0_mip_fallback = options.world_channels;
    decode_options.decode_mip_levels = options.world_channels && options.world_mip;
    decode_options.mip_registers_known = mip_registers_known;
    decode_options.mip_bp1 = miptbp1;
    decode_options.mip_bp2 = miptbp2;
    if (decode_texture_state(image, state, decode_options, decoded, decode_error) != TextureStatus::ok) {
      counters.world_state_refused += 1;
      if (counters.state_refusals.size() < 16) {
        char line[256];
        std::snprintf(line, sizeof line, "TEX0 %016llX TEX1 %016llX CLAMP %016llX TEX2 %016llX: %s",
                      static_cast<unsigned long long>(state.tex0), static_cast<unsigned long long>(state.tex1),
                      static_cast<unsigned long long>(state.clamp),
                      static_cast<unsigned long long>(state.tex2), decode_error.c_str());
        counters.state_refusals.push_back(line);
      }
      return nullptr;
    }
    counters.records_decoded += 1;
    if (decoded.level0_mip_fallback) counters.records_level0_fallback += 1;
    if (decoded.mip_levels_missing != 0u) counters.records_mip_levels_missing += 1;
    if (!decoded.mip_levels.empty()) counters.records_mip_chains += 1;
    if (decoded.palette_via_tex2) counters.world_state_palette_tex2 += 1;
    const std::uint32_t slot = payload_slot(decoded);
    table.textures.push_back(make_sampler(decoded, slot, true, &state));
    const SampledTexture *result = &table.textures.back();
    state_cache.emplace_back(key, result);
    return result;
  };

  // TASK-090 texture mip: the explicit per-vertex LOD of one item (chapter 01.5/03.6). It needs a complete chain and
  // the item's tapped STQ Q; a chain without Q keeps the counted level-0 fallback. The LOD is the GS formula
  // K/16 - log2|Q|*2^L (LCM = 0) or K/16 (LCM = 1), which is what the reference's CoreGS 2.0 shader samples with
  // (hw_pixel_uber.frag, sample_mipmapped); the level is that value clamped to [0, MXL], not a value relative to a
  // selected base level (the base-level rule belongs to the GPU image layout, not to the sampled LOD).
  const auto attach_mip_lod = [&](TextureBinding &binding, const TapStream *stream) {
    if (!options.world_mip || binding.texture == nullptr || !binding.texture->mip_ready) return;
    binding.mip_base_level = 0;
    if (stream == nullptr || stream->stq.empty()) {
      counters.world_mip_lod_missing += 1;
      return;
    }
    hwgs::lod::Tex1Fields fields;
    fields.lcm = binding.texture->lcm != 0;
    fields.mxl = binding.texture->mxl;
    fields.mmag_linear = binding.texture->mmag != 0;
    fields.mmin = binding.texture->mmin;
    fields.mtba = false;
    fields.l = binding.texture->lod_l;
    fields.k_raw = binding.texture->lod_k_raw;
    binding.vertex_lod.assign(stream->stq.size(), 0.0f);
    bool any = false;
    for (std::size_t index = 0; index < stream->stq.size(); ++index) {
      if (stream->stq[index].valid == 0) continue;
      const double lod =
          hwgs::lod::lod_from_q(stream->stq[index].q, fields.l, fields.k_raw, fields.lcm);
      binding.vertex_lod[index] = static_cast<float>(lod);
      counters.world_mip_lod_min = std::min(counters.world_mip_lod_min, lod);
      counters.world_mip_lod_max = std::max(counters.world_mip_lod_max, lod);
      counters.world_mip_lod_sum += lod;
      counters.world_mip_q_min = std::min(counters.world_mip_q_min, static_cast<double>(stream->stq[index].q));
      counters.world_mip_q_max = std::max(counters.world_mip_q_max, static_cast<double>(stream->stq[index].q));
      counters.world_mip_lod_vertices += 1;
      any = true;
    }
    if (!any) {
      counters.world_mip_lod_missing += 1;
      return;
    }
    counters.world_mip_lod_items += 1;
  };

  // ---- 4. Bind every item and attach its tapped state.
  // TASK-090 pass textures: a world-pass item decodes against the pixels its own VU1 job sampled, falling back to its
  // draw's K1 record, then its pass-begin image, then the frame-end default image, each step counted. Non-world items
  // (the car pass) always use the default image, exactly as before.
  // 0: the pass/default image, 1: the draw's K1 record, 2: the draw's VU1 job record.
  const auto note_world_image = [&](std::uint32_t pass_image, int overlay_kind) {
    if (overlay_kind == 2) {
      counters.world_job_slm += 1;
      return;
    }
    if (overlay_kind == 1) {
      counters.world_draw_slm += 1;
      return;
    }
    if (options.pass_slms == nullptr) return;
    if (pass_image != 0)
      counters.world_pass_slm += 1;
    else
      counters.world_pass_slm_missing += 1;
  };
  if (options.draw_textures != nullptr && options.draw_textures->records != nullptr)
    counters.world_draw_slm_records = options.draw_textures->records->size();
  if (options.job_textures != nullptr && options.job_textures->records != nullptr)
    counters.world_job_slm_records = options.job_textures->records->size();
  // TASK-090 pass textures: the overlay image is the default image with one record's windows patched in. Only one
  // overlay is applied at a time: switching records restores the previous windows from the default first, so one
  // draw's content never leaks into another's decode.
  SlmImage working;
  bool working_ready = false;
  const VramRecord *applied_overlay = nullptr;
  const auto apply_overlay = [&](const VramRecord *overlay) -> const SlmImage * {
    if (overlay == nullptr) return nullptr;
    if (overlay == applied_overlay) return &working;
    if (!working_ready) {
      working = slm;
      working_ready = true;
    }
    if (applied_overlay != nullptr) {
      working.covered = slm.covered;
      for (const vram::Window &window : applied_overlay->windows) {
        const std::size_t offset = static_cast<std::size_t>(window.first_block) * vram::kBlockBytes;
        std::memcpy(working.local.bytes().data() + offset, slm.local.bytes().data() + offset,
                    static_cast<std::size_t>(window.block_count) * vram::kBlockBytes);
      }
    }
    applied_overlay = nullptr;
    if (!apply_vram_record(*overlay, working, error)) {
      if (overlay->kind == vram::kRecordJobTexture)
        counters.world_job_slm_refused += 1;
      else
        counters.world_draw_slm_refused += 1;
      return nullptr;
    }
    applied_overlay = overlay;
    if (overlay->kind != vram::kRecordJobTexture) counters.world_draw_slm_draws += 1;
    return &working;
  };
  for (std::size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    const std::uint32_t draw_image =
        options.pass_slms != nullptr && is_world_pass(draw.pass) && draw.pass < kPassCount ? pass_slot[draw.pass] : 0;
    const SlmImage *draw_slm = image_slms[draw_image];
    const std::uint32_t draw_image_id = draw_image;
    std::vector<TextureBinding> &bindings = table.draws[draw_index];
    bindings.resize(draw.item_count);
    std::size_t cursor = 0;
    // TASK-090 track M1: the last exact-UV-paired state of this draw, for the items whose own record cannot be
    // verified. The measured course items of one draw share their GS state (the per-item census: 1,226 of 1,284 items
    // carry the same TEST/ALPHA), so the neighbouring measured state is a closer default than the pass-begin journal.
    SampleState last_course_state;
    bool have_last_course_state = false;
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      TextureBinding &binding = bindings[index];
      binding.state = measured_sample_state();
      binding.texture_record = item.texture_record;
      counters.items_visited += 1;
      // TASK-090 pass textures: the item's own VU1 job record first (the content its draw sampled), then its draw's K1
      // record (evidence; it predates the frame's uploads), then the pass image and the default.
      const VramRecord *item_overlay = nullptr;
      if (is_world_pass(draw.pass) && options.job_textures != nullptr &&
          options.job_textures->records != nullptr) {
        const auto address = job_of_address.find(item.address);
        if (address != job_of_address.end()) {
          const auto found = options.job_textures->records->find(address->second);
          if (found != options.job_textures->records->end()) item_overlay = found->second;
        }
      }
      if (item_overlay == nullptr && is_world_pass(draw.pass) && options.draw_textures != nullptr &&
          options.draw_textures->records != nullptr) {
        const auto found = options.draw_textures->records->find(draw.sequence);
        if (found != options.draw_textures->records->end()) item_overlay = found->second;
      }
      const SlmImage *item_slm = draw_slm;
      std::uint32_t item_image_id = draw_image_id;
      int overlay_kind = 0;
      if (item_overlay != nullptr) {
        if (const SlmImage *overlay_image = apply_overlay(item_overlay)) {
          item_slm = overlay_image;
          overlay_kind = item_overlay->kind == vram::kRecordJobTexture ? 2 : 1;
          item_image_id = overlay_kind == 2 ? (0x80000000u | item_overlay->label)
                                            : (0x40000000u | draw.sequence);
        }
      }

      // The item's tapped state, when the tap paired it. The pairing advances across the item list in order.
      std::size_t chosen = 0;
      bool matched = false;
      bool ambiguous_pick = false;  // the best UV match tied with another stream (a texture-binding risk)
      // TASK-090 track textures: the stream the texture state is taken from. It is `chosen` unless the UV match tied:
      // then the tied stream whose TEX0 matches the item's record is preferred, because a tie carries no other signal.
      std::size_t texture_pick = 0;
      bool have_texture_pick = false;
      if (!streams.empty() && item.packet_bytes != 0 && counts[draw_index][index] != 0) {
        const bool has_record = item.texture_record != kNoRecord && item.texture_record < frame.textures.size();
        std::uint32_t item_tbp0 = 0;
        std::uint32_t item_psm = 0;
        if (has_record) {
          const std::uint64_t tex0 = vram::read_u64(frame.textures[item.texture_record].bytes, 0);
          item_tbp0 = bits(tex0, 0, 13);
          item_psm = bits(tex0, 20, 25);
        }
        if (options.world_channels) {
          // TASK-090 track M1: the world items pair by the measured UV equality against every stream of the frame (the
          // item-state study's rule): the record whose tapped S/Q,T/Q best equals the item's model UV at the same vertex
          // index. The streams are not consumed, because a tap record can belong to more than one captured item.
          std::vector<std::array<float, 2>> uvs;
          // Format version 6: a 0x00A0 item's tapped S/Q,T/Q is its post-morph UV, so the pairing compares the blend.
          read_item_uv(frame.packets.data(), frame.packets.size(), item, 16, uvs, morph_blend_of(draw));
          double best_error = 1e30;
          std::size_t best_stream = 0;
          std::uint32_t best_candidates = 0;
          std::vector<std::size_t> best_ties;
          for (std::size_t stream = 0; stream < streams.size(); ++stream) {
            if (streams[stream].vertices != counts[draw_index][index]) continue;
            double error = 0.0;
            std::size_t samples = 0;
            const std::size_t count = std::min(uvs.size(), streams[stream].stq.size());
            for (std::size_t vertex = 0; vertex < count; ++vertex) {
              if (streams[stream].stq[vertex].valid == 0) continue;
              error += std::fabs(streams[stream].stq[vertex].u - uvs[vertex][0]) +
                       std::fabs(streams[stream].stq[vertex].v - uvs[vertex][1]);
              ++samples;
            }
            if (samples == 0) continue;
            if (error < best_error) {
              best_error = error;
              best_stream = stream;
              best_candidates = 1;
              best_ties.clear();
              best_ties.push_back(stream);
            } else if (error == best_error) {
              best_candidates += 1;
              best_ties.push_back(stream);
            }
          }
          // The item-state study accepts the best UV match; the summed residual is only a tie-breaker. A best match with
          // no UV sample at all cannot be verified and stays a counted default (the item is then excluded from the
          // selected world picture), never a guessed XYOFFSET/TEST.
          if (best_error <= 4.0) {
            binding.state = streams[best_stream].state;
            chosen = best_stream;
            matched = true;
            ambiguous_pick = best_candidates > 1;
            counters.tap_items_paired += 1;
            if (best_candidates > 1) counters.tap_items_ambiguous += 1;
            // TASK-090 track textures: the texture state of a tied UV match. Repeated quad UVs make several streams
            // equally good; among them, prefer the one whose TEX0 (the load-control CLD bit ignored) equals the item's
            // record. With no such stream the first tied stream keeps the state and the tie is counted.
            texture_pick = best_stream;
            have_texture_pick = true;
            const bool world_pass = draw.pass == kPassSky || draw.pass == kPassFarUnderlay ||
                                    draw.pass == kPassCourse || draw.pass == kPassReflection;
            if (world_pass && best_candidates > 1) {
              const auto sampling_tex0 = [](std::uint64_t tex0) { return tex0 & ~(7ull << 61); };
              const std::uint64_t record_tex0 =
                  has_record ? vram::read_u64(frame.textures[item.texture_record].bytes, 0) : 0;
              bool record_match = false;
              if (has_record) {
                for (const std::size_t candidate : best_ties) {
                  if (!streams[candidate].texture.tex0_known) continue;
                  if (sampling_tex0(streams[candidate].texture.tex0) == sampling_tex0(record_tex0)) {
                    texture_pick = candidate;
                    record_match = true;
                    break;
                  }
                }
              }
              if (record_match)
                counters.world_texture_pair_tiebreak += 1;
              else
                counters.world_texture_pair_ambiguous += 1;
            }
            if (draw.pass == kPassCourse) {
              last_course_state = binding.state;
              have_last_course_state = true;
            }
          } else {
            counters.tap_items_defaulted += 1;
          }
        } else {
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
        }
      } else if (!streams.empty()) {
        counters.tap_items_defaulted += 1;
      }
      if (options.world_channels && matched && binding.vertex_fog.empty())
        binding.vertex_fog = streams[chosen].fog;
      if (options.world_channels && !matched && draw.pass == kPassCourse) {
        // No exact-UV record for this item (a split/8-bit UV array or a record without STQ): use the draw's last paired
        // course state, or the course pass journal when the draw has none, and count the item. A mirror-view item is
        // excluded by this rule only when its per-item state was paired; the unverified items are selected as main view
        // on the measured neighbour/pass state.
        if (have_last_course_state) {
          binding.state = last_course_state;
          binding.state.world_pass_default = true;
        } else {
          binding.state = world_pass_default_state(frame, kPassCourse);
        }
        if (binding.state.world_pass_default) counters.tap_items_uv_unverified += 1;
      }
      if (options.world_channels && draw.pass == kPassCourse) {
        // The course pass's TEST/ALPHA come from the K7 pass journal. The tap re-emits duplicate records of the same
        // geometry with different TEST states (the measured duplicate-record trap), so a per-item UV match can land on
        // the alpha-tested duplicate (TEST 0x5820D, ATE GREATER 32) while the pass runs TEST 0x5800E (ATE off); with the
        // CT32 palette's alpha 0 that rejects the road the reference shows. The journal is the measured pass-begin state
        // and is used for these two write-once registers; PRIM/FBMSK/ZMSK and the per-vertex fog stay per item.
        const SampleState journal = world_pass_default_state(frame, kPassCourse);
        if (journal.world_pass_default) {
          // Preserve the paired record's zero-TEST fact (the 3 measured no-state items) before the journal replaces the
          // field, so the selection rule can still exclude them.
          if (binding.state.test_known && binding.state.test == 0) binding.state.world_test_zero = true;
          if (journal.test_known) {
            binding.state.test = journal.test;
            binding.state.test_known = true;
          }
          counters.tap_items_course_journal_state += 1;
        }
      }

      // TASK-090 track textures: the world passes (sky, far underlay, course) bind the GS state the tap measured, not
      // the 40-byte record. The record stays the fallback only when the tap carried no state at all; a tapped state the
      // decoder refuses stays a counted refusal (never a silent record substitution), and a tapped state can name a
      // texture the capture's record table does not hold (the external cursor case).
      const bool world_pass_item =
          options.world_channels &&
          (draw.pass == kPassSky || draw.pass == kPassFarUnderlay || draw.pass == kPassCourse ||
           draw.pass == kPassReflection);
      const bool has_record = item.texture_record != kNoRecord && item.texture_record < frame.textures.size();
      if (world_pass_item) {
        // The record-vs-tap census: the tapped TEX0 (CLD ignored, because the world stream clears it and loads the
        // palette through TEX2) and the tapped TEX1/CLAMP registers against the item's 40-byte record.
        if (have_texture_pick) {
          const TextureState &texture_state = streams[texture_pick].texture;
          if (!has_record) {
            counters.world_tap_record_absent += 1;
          } else if (texture_state.tex0_known) {
            TextureRecord record_fields;
            std::string parse_error;
            if (parse_texture_record(frame.textures[item.texture_record].bytes,
                                     sizeof frame.textures[item.texture_record].bytes, record_fields, parse_error)) {
              const bool page_differs = hwgs::tex0::Tbp0(texture_state.tex0) != record_fields.tbp0;
              const bool format_differs =
                  hwgs::tex0::Tbw(texture_state.tex0) != record_fields.tbw ||
                  hwgs::tex0::Psm(texture_state.tex0) != record_fields.psm ||
                  hwgs::tex0::Tw(texture_state.tex0) != record_fields.tw ||
                  hwgs::tex0::Th(texture_state.tex0) != record_fields.th ||
                  hwgs::tex0::Tcc(texture_state.tex0) != record_fields.tcc ||
                  hwgs::tex0::Tfx(texture_state.tex0) != record_fields.tfx;
              const bool palette_differs =
                  hwgs::tex0::Cbp(texture_state.tex0) != record_fields.cbp ||
                  hwgs::tex0::Cpsm(texture_state.tex0) != record_fields.cpsm ||
                  hwgs::tex0::Csm(texture_state.tex0) != record_fields.csm ||
                  hwgs::tex0::Csa(texture_state.tex0) != record_fields.csa;
              if (page_differs || format_differs || palette_differs) {
                counters.world_tap_tex0_differs += 1;
                if (page_differs) counters.world_tap_tex0_tbp_differs += 1;
                if (format_differs) counters.world_tap_tex0_format_differs += 1;
                if (palette_differs) counters.world_tap_tex0_palette_differs += 1;
                if (ambiguous_pick) counters.world_tap_diff_ambiguous += 1;
              }
              if (texture_state.tex1_known && texture_state.tex1 != record_fields.tex1) {
                counters.world_tap_tex1_differs += 1;
                if (ambiguous_pick) counters.world_tap_diff_ambiguous += 1;
              }
              if (texture_state.clamp_known && texture_state.clamp != record_fields.clamp) {
                counters.world_tap_clamp_differs += 1;
                if (ambiguous_pick) counters.world_tap_diff_ambiguous += 1;
              }
            }
          }
        }
        // The world path's texture binding.
        const LightProgram program = light_program_of_item(item);
        if (program == LightProgram::body_paint) {
          // The world passes never sample the body/paint texgen map; keep the measured deferral.
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
        const TextureState *tapped = have_texture_pick ? &streams[texture_pick].texture : nullptr;
        if (tapped == nullptr) counters.world_texture_pair_none += 1;
        const bool state_present = tapped != nullptr && tapped->any_register_known();
        if (tapped != nullptr && tapped->sampling_registers_known()) {
          const SampledTexture *sampler =
              decode_state(*tapped, *item_slm, item_image_id,
                           has_record ? &frame.textures[item.texture_record] : nullptr);
          if (sampler == nullptr) {
            counters.items_decode_refused += 1;
            counters.items_untextured += 1;
            continue;
          }
          binding.texture = sampler;
          if (have_texture_pick) {
            binding.vertex_q.resize(streams[texture_pick].stq.size());
            for (std::size_t vertex = 0; vertex < streams[texture_pick].stq.size(); ++vertex)
              binding.vertex_q[vertex] = streams[texture_pick].stq[vertex].q;
          }
          attach_mip_lod(binding, have_texture_pick ? &streams[texture_pick] : nullptr);
          note_world_image(draw_image, overlay_kind);
          counters.world_state_decoded += 1;
          counters.items_textured += 1;
          continue;
        }
        if (state_present) {
          // A tapped state that is missing one of the sampling registers: counted refusal, never a record substitute.
          counters.world_state_refused += 1;
          if (counters.state_refusals.size() < 16)
            counters.state_refusals.push_back("incomplete tapped texture state (TEX0/TEX1/CLAMP)");
          counters.items_decode_refused += 1;
          counters.items_untextured += 1;
          continue;
        }
        // No tapped state: the 40-byte record fallback.
        if (!has_record) {
          counters.items_no_texture += 1;
          counters.items_untextured += 1;
          continue;
        }
        counters.world_record_fallback += 1;
        const SampledTexture *sampler =
            decode_record(item.texture_record, frame.textures[item.texture_record], *item_slm, item_image_id);
        if (sampler == nullptr) {
          counters.items_decode_refused += 1;
          counters.items_untextured += 1;
          continue;
        }
        binding.texture = sampler;
        attach_mip_lod(binding, nullptr);
        note_world_image(draw_image, overlay_kind);
        counters.items_textured += 1;
        continue;
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
        const SampledTexture *sampler =
            decode_record(item.texture_record, frame.textures[item.texture_record], slm, 0u);
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
      const SampledTexture *sampler = decode_record(item.texture_record, frame.textures[item.texture_record], slm, 0u);
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
