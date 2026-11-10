// TASK-090 CoreGS 3.0: the per-frame texture table of the offline reference renderer.
//
// The `.gt4scene` capture names, per item, a 40-byte PGLUtexture record; the `.gt4vram` sidecar holds the GS local
// memory the record points at; the `.gt4tap` tap holds the A+D state records the VU1 emitted for the draw (TEST, ALPHA,
// FRAME, ZBUF, the PRIM in force and, for the world passes, the TEX0/TEX1/CLAMP/TEX2 writes). This unit joins them into
// one `FrameTextureTable`:
//
//   * it decodes every texture record the frame's items use, once per record, and shares the decoded RGBA8 between
//     records that produce the same Format G content key (the decoder has to run to compute the key, so the cache
//     avoids the duplicate bytes and the duplicate sampler state, not the second decode pass);
//   * it binds, per item, the SampledTexture the offline sampler reads, or no texture with a counted reason (no record,
//     no model UV channel, a refused decode, or the body/paint texgen deferral of this milestone);
//   * TASK-090 track textures: for the world passes (sky, far underlay, course) it decodes the texture from the tapped
//     TEX0/TEX1/CLAMP/TEX2 state at the item's draw, not from the 40-byte record; the record is the fallback only when
//     the tap carried no state, and every refused state is counted. The texture state of a UV tie prefers the tied
//     stream whose TEX0 matches the item's record (repeated quad UVs make the global UV search ambiguous for the page);
//   * it attaches the tap's per-item SampleState (TEST/ALPHA/FRAME/ZBUF/PRIM) by the proven pairing of the `uv` gate:
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
  std::uint64_t records_decoded = 0;   // decode calls that actually ran (record- or state-path cache misses)
  std::uint64_t records_refused = 0;   // records the decoder refused
  std::uint64_t records_level0_fallback = 0;  // TEX1.MXL > 0 decoded as level 0 (the world path's declared fallback)
  std::uint64_t records_filter_fallback = 0;  // MMIN is a mip filter but only level 0 exists: nearest (MMIN & 1) is used
  std::uint64_t cache_hits = 0;        // decodes that shared an already decoded Format G payload key
  std::uint64_t items_textured = 0;
  std::uint64_t items_untextured = 0;
  std::uint64_t items_texgen_deferred = 0;  // body/paint: its ST is texgen ST, not the model UV (pre-reflection mode)
  std::uint64_t items_reflected = 0;        // body/paint items bound as their environment-map reflection pass
  std::uint64_t items_reflection_no_state = 0;  // body/paint items with no usable tapped reflection record
  std::uint64_t reflection_fog_missing = 0;     // reflection records whose vertices carried no fog value
  std::uint64_t items_no_texture = 0;       // the item names no texture record
  std::uint64_t items_no_uv = 0;            // the item carries no model UV channel
  std::uint64_t items_decode_refused = 0;   // the texture record was refused by the decoder
  std::uint64_t items_program_unknown = 0;  // the item's VU1 program is not measured (it is still sampled)
  std::uint64_t tap_items_paired = 0;       // items that received a tapped state snapshot
  std::uint64_t tap_items_defaulted = 0;    // items with no matching tapped vertex record
  std::uint64_t tap_items_uv_unverified = 0;  // world items with no exact-UV record: state taken from the pass journal
  std::uint64_t tap_items_course_journal_state = 0;  // course items whose TEST/ALPHA came from the pass journal
  std::uint64_t tap_items_ambiguous = 0;    // pairings with more than one candidate vertex record
  std::uint64_t tap_records = 0;            // tap records walked
  std::uint64_t tap_state_writes = 0;       // A+D writes applied to the tracked state
  // TASK-090 track textures: the world passes (sky/far-underlay/course) bind the GS state the tap measured, not the
  // 40-byte record. These count the decode source and the record-vs-tap differences over those items.
  std::uint64_t world_state_decoded = 0;      // world items decoded from the tapped TEX0/TEX1/CLAMP(/TEX2) state
  std::uint64_t world_state_refused = 0;      // world items whose tapped state the decoder refused (counted, untextured)
  std::uint64_t world_record_fallback = 0;    // world items with no tapped state: decoded from the 40-byte record
  std::uint64_t world_state_palette_tex2 = 0; // state decodes whose palette bank came from the TEX2 load (CLD = 0)
  // The texture-state pairing counts. The world selection's per-item state keeps the measured global UV pairing; the
  // texture state uses the same UV rule, but when several streams tie on the best UV residual (repeated quad UVs are
  // common on the course) it prefers the tied stream whose TEX0 matches the item's record, because a tie carries no
  // other measured signal. A tie with no record-matching stream keeps the first stream and is counted.
  std::uint64_t world_texture_pair_tiebreak = 0;  // texture state from a tied stream that matched the item's record
  std::uint64_t world_texture_pair_ambiguous = 0; // a tie with no record-matching stream: the first stream was used
  std::uint64_t world_texture_pair_none = 0;      // no tapped stream: the record fallback
  std::uint64_t world_tap_tex0_differs = 0;   // tapped TEX0 differs from the record's (CLD ignored)
  std::uint64_t world_tap_tex0_tbp_differs = 0;      // the texture page (TBP0) differs
  std::uint64_t world_tap_tex0_format_differs = 0;   // PSM/TBW/TW/TH/TCC/TFX differ
  std::uint64_t world_tap_tex0_palette_differs = 0;  // CBP/CPSM/CSM/CSA differ
  std::uint64_t world_tap_tex1_differs = 0;          // the tapped TEX1 register differs from the record's
  std::uint64_t world_tap_clamp_differs = 0;         // the tapped CLAMP register differs from the record's
  std::uint64_t world_tap_diff_ambiguous = 0;        // a differing item whose UV pairing had more than one candidate
  std::uint64_t world_tap_record_absent = 0;  // a tapped state names a texture with no capture record (external cursor)
  std::vector<std::string> state_refusals;    // the first few tapped-state refusals, in words (bounded)
};

struct TextureFrameOptions {
  bool use_tap = false;       // false: every item keeps the measured default state and the counters say so
  bool texture_body = false;  // diagnostic only: let body/paint items through with the model UV (not the milestone)
  // TASK-090 reflection milestone: bind each body/paint item as its measured environment-map reflection pass (the
  // pinned texgen S,T instead of the model UV), carrying the tapped PRIM 0x7C state and per-vertex fog. False keeps
  // the pre-reflection deferral (the body items stay on the lit colour).
  bool reflections = false;
  // TASK-090 track M1: the world path. The per-item vertex count is the TOPS 0xC0 descriptor count (the tool has already
  // filtered the draws/items), V2-8 and the continuation UV arrays are accepted, and a record with TEX1.MXL > 0 is
  // decoded at level 0 with a counted fallback. False keeps the car pass's measured rule exactly.
  bool world_channels = false;
};

// Builds the table for `frame` from the decoded sidecar image `slm` and (when options.use_tap) the tap file. `tap_data`
// and `tap_records` come from read_tap_file; both may be empty. Returns false only when the frame itself cannot be
// walked (a shape packet does not decode); per-item refusals are counted, not fatal.
bool build_frame_texture_table(const FrameSnapshot &frame, const SlmImage &slm, const std::uint8_t *tap_data,
                               const std::vector<TapFileRecord> &tap_records, const TextureFrameOptions &options,
                               FrameTextureTable &table, TextureFrameCounters &counters, std::string &error);

}  // namespace gt4::coregs3
