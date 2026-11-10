// TASK-090 CoreGS 3.0: the per-frame texture table of the offline reference renderer.
//
// The `.gt4scene` capture names, per item, a 40-byte PGLUtexture record; the `.gt4vram` sidecar holds the GS local
// memory the record points at; the `.gt4tap` tap holds the A+D state records the VU1 emitted for the draw (TEST, ALPHA,
// FRAME, ZBUF, TEX0 and the PRIM in force). This unit joins the three into one `FrameTextureTable`:
//
//   * it decodes every texture record the frame's items use, once per record, and shares the decoded RGBA8 between
//     records that produce the same Format G content key (the decoder has to run to compute the key, so the cache
//     avoids the duplicate bytes and the duplicate sampler state, not the second decode pass);
//   * it binds, per item, the SampledTexture the offline sampler reads, or no texture with a counted reason (no record,
//     no model UV channel, a refused decode, or the body/paint texgen deferral of this milestone);   * it attaches the tap's per-item SampleState (TEST/ALPHA/FRAME/ZBUF/PRIM) by the proven pairing of the `uv` gate:
//     walk the tap records of the draw's jobs in emission order, keep the last A+D register writes as state, and give
//     each item the next vertex record whose vertex count and TEX0 (TBP0, PSM) match. A field the tap never wrote, and
//     the whole state when no tap is supplied, keeps the measured car-pass default and is counted.
//
// The tap's source records carry the capture frame's VBlank for the jobs it posted (the PATH1 GIF records themselves
// arrive about three VBlanks later on this machine), which is how a tapped job is tied to one captured frame.
//
// Every refusal is explicit and counted. This unit is built only with the native decoder (GT4_HW_GS), because it calls
// it; the sampler it feeds (coregs3_sampler.hpp) is always built.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coregs3_sampler.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_texture.hpp"
#include "coregs3_tap_file.hpp"

namespace gt4::coregs3 {

struct TextureFrameCounters {
  std::uint64_t items_visited = 0;
  std::uint64_t records_decoded = 0;   // cache misses: records actually decoded
  std::uint64_t records_refused = 0;   // records the decoder refused
  std::uint64_t cache_hits = 0;        // records that shared an already decoded Format G key
  std::uint64_t items_textured = 0;
  std::uint64_t items_untextured = 0;
  std::uint64_t items_texgen_deferred = 0;  // body/paint: its ST is texgen ST, not the model UV (milestone deferral)
  std::uint64_t items_no_texture = 0;       // the item names no texture record
  std::uint64_t items_no_uv = 0;            // the item carries no model UV channel
  std::uint64_t items_decode_refused = 0;   // the texture record was refused by the decoder
  std::uint64_t items_program_unknown = 0;  // the item's VU1 program is not measured (it is still sampled)
  std::uint64_t tap_items_paired = 0;       // items that received a tapped state snapshot
  std::uint64_t tap_items_defaulted = 0;    // items with no matching tapped vertex record
  std::uint64_t tap_items_ambiguous = 0;    // pairings with more than one candidate vertex record
  std::uint64_t tap_records = 0;            // tap records walked
  std::uint64_t tap_state_writes = 0;       // A+D writes applied to the tracked state
};

struct TextureFrameOptions {
  bool use_tap = false;       // false: every item keeps the measured default state and the counters say so
  bool texture_body = false;  // diagnostic only: let body/paint items through with the model UV (not the milestone)
};

// Builds the table for `frame` from the decoded sidecar image `slm` and (when options.use_tap) the tap file. `tap_data`
// and `tap_records` come from read_tap_file; both may be empty. Returns false only when the frame itself cannot be
// walked (a shape packet does not decode); per-item refusals are counted, not fatal.
bool build_frame_texture_table(const FrameSnapshot &frame, const SlmImage &slm, const std::uint8_t *tap_data,
                               const std::vector<TapFileRecord> &tap_records, const TextureFrameOptions &options,
                               FrameTextureTable &table, TextureFrameCounters &counters, std::string &error);

}  // namespace gt4::coregs3
