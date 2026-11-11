// TASK-090 CoreGS 3.0, phase S2: the offline decoder and census of the `.gt4gs` PATH2/PATH3 stream.
//
// The capture (S1) holds every GIF byte the game sends on PATH2/PATH3, but a raw byte stream answers no question until
// it is walked the way the GS walks it. This unit turns the records into the packet classes the S2 layer needs: IMAGE
// uploads with their BITBLTBUF/TRXPOS/TRXREG/TRXDIR state, register writes (PACKED descriptors, REGLIST words and A+D),
// and primitive batches (the vertex runs of one tag under one PRIM, with the GS state in force at the first vertex).
//
// The walker is the measured CoreGS 2.0 GIF semantics (native/hw_gs/frontend/hw_gif_parser.cpp), written independently
// here because this unit lives in gt4_coregs3, which is built without GT4_HW_GS:
//   * PACKED: NLOOP x NREG qwords, one descriptor per qword; RGBA/ST/UV/XYZ have the PACKED layouts of
//     coregs3_gif.hpp; descriptor 0xE is a register write whose address is the high qword's low 7 bits and whose data
//     is the low qword in the canonical 64-bit register layout (vertices included, with the register XYZ layout).
//   * REGLIST: NLOOP x NREG 64-bit register words, two per qword (low word first, an odd final word padded); every word
//     uses the canonical register layout and the descriptor advances per word. NLOOP is the number of loop iterations,
//     NOT the number of words (the S2 measurement that first looked like a shifted stream).
//   * IMAGE: NLOOP qwords of transfer data. A packet may start in one capture record and its data may continue in the
//     next (the S1 capture records the EE's DMA transfers, which split at 16-byte boundaries), so the walker is a
//     per-path state machine across records.
//   * Packet framing: the tag's EOP bit ends the packet; a capture record usually holds one packet, sometimes a register
//     packet followed by the IMAGE tag of an upload whose data is the next record.
//
// The screen position uses the measured 12.4 wrap rule: dx = (x - XYOFFSET.OFX) & 0xFFFF, sx = dx / 16, and sx is the
// signed 12-bit pixel coordinate (subtract 4096 above 2047.9375). That is the GS arithmetic; it is not a sign extension
// of the 16-bit vertex word.
//
// Attribution: S1 measured (Confidence C) that a frame's stream is kicked at the start of the NEXT VBlank interval: the
// packets stamped V are the display list that frame V-1's passes built. This census therefore reports both the capture
// VBlank and the attributed scene frame `V - 1`, and no packet is ever placed by acceptance-time pass (every S1 packet
// was `pass = unknown`). The layer selection uses the same relation.
//
// Every packet is counted in exactly one format class (packed/reglist/image/disabled), and within it in the content
// classes (register write, primitive, empty); a packet the walker cannot finish is a named refusal, never a silent skip.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "coregs3_gsstream_file.hpp"

namespace gt4::coregs3 {

// The canonical per-path register file the walker maintains. Values are the raw 64-bit register words; the helpers decode
// the measured bit fields. A field the stream never wrote keeps its reset value (0), which the caller can tell apart via
// the *_known flags the census keeps per register address.
struct GsDecodeState {
  std::uint64_t prim = 0;
  std::uint64_t rgba = 0;       // canonical RGBAQ: R 0-7, G 8-15, B 16-23, A 24-31, Q 32-63 (float bits)
  std::uint64_t st = 0;         // S 0-31, T 32-63 (float bits)
  std::uint64_t uv = 0;         // U 0-15, V 16-31 (12.4 texel units in the register layout)
  std::uint64_t tex0 = 0;
  std::uint64_t tex1 = 0;
  std::uint64_t clamp = 0;
  std::uint64_t fog = 0;
  std::uint64_t fogcol = 0;
  std::uint64_t frame = 0;
  std::uint64_t zbuf = 0;
  std::uint64_t test = 0;
  std::uint64_t alpha = 0;
  std::uint64_t scissor = 0;
  std::uint64_t xyoffset = 0;
  std::uint64_t pabe = 0;

  std::uint32_t prim_value() const noexcept { return static_cast<std::uint32_t>(prim & 0x7FFu); }
  std::uint32_t prim_type() const noexcept { return prim_value() & 7u; }
  bool prim_abe() const noexcept { return ((prim_value() >> 6) & 1u) != 0u; }
  bool prim_tme() const noexcept { return ((prim_value() >> 4) & 1u) != 0u; }
  bool prim_fge() const noexcept { return ((prim_value() >> 5) & 1u) != 0u; }
  std::uint32_t frame_fbp() const noexcept { return static_cast<std::uint32_t>(frame & 0x1FFu); }
  std::uint32_t frame_fbw() const noexcept { return static_cast<std::uint32_t>((frame >> 16) & 0x3Fu); }
  std::uint32_t frame_psm() const noexcept { return static_cast<std::uint32_t>((frame >> 24) & 0x3Fu); }
  std::uint32_t frame_fbmsk() const noexcept { return static_cast<std::uint32_t>((frame >> 32) & 0xFFFFFFFFu); }
  std::uint32_t tex0_tbp0() const noexcept { return static_cast<std::uint32_t>(tex0 & 0x3FFFu); }
  std::uint32_t tex0_psm() const noexcept { return static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu); }
  std::uint32_t tex0_tw() const noexcept { return static_cast<std::uint32_t>((tex0 >> 26) & 0xFu); }
  std::uint32_t tex0_th() const noexcept { return static_cast<std::uint32_t>((tex0 >> 30) & 0xFu); }
  std::uint32_t tex0_tcc() const noexcept { return static_cast<std::uint32_t>((tex0 >> 34) & 1u); }
  std::uint32_t tex0_tfx() const noexcept { return static_cast<std::uint32_t>((tex0 >> 35) & 3u); }
  std::uint32_t tex1_mmag() const noexcept { return static_cast<std::uint32_t>((tex1 >> 5) & 1u); }
  std::uint32_t tex1_mmin() const noexcept { return static_cast<std::uint32_t>((tex1 >> 6) & 7u); }
  std::uint32_t tex1_mxl() const noexcept { return static_cast<std::uint32_t>((tex1 >> 2) & 7u); }
  std::uint32_t clamp_wms() const noexcept { return static_cast<std::uint32_t>(clamp & 3u); }
  std::uint32_t clamp_wmt() const noexcept { return static_cast<std::uint32_t>((clamp >> 2) & 3u); }
  std::uint32_t clamp_minu() const noexcept { return static_cast<std::uint32_t>((clamp >> 4) & 0x3FFu); }
  std::uint32_t clamp_maxu() const noexcept { return static_cast<std::uint32_t>((clamp >> 14) & 0x3FFu); }
  std::uint32_t clamp_minv() const noexcept { return static_cast<std::uint32_t>((clamp >> 24) & 0x3FFu); }
  std::uint32_t clamp_maxv() const noexcept { return static_cast<std::uint32_t>((clamp >> 34) & 0x3FFu); }
  std::uint32_t test_ate() const noexcept { return static_cast<std::uint32_t>(test & 1u); }
  std::uint32_t test_ztst() const noexcept { return static_cast<std::uint32_t>((test >> 17) & 3u); }
  std::uint32_t zbuf_zmsk() const noexcept { return static_cast<std::uint32_t>((zbuf >> 32) & 1u); }
  std::uint32_t alpha_a() const noexcept { return static_cast<std::uint32_t>(alpha & 3u); }
  std::uint32_t alpha_b() const noexcept { return static_cast<std::uint32_t>((alpha >> 2) & 3u); }
  std::uint32_t alpha_c() const noexcept { return static_cast<std::uint32_t>((alpha >> 4) & 3u); }
  std::uint32_t alpha_d() const noexcept { return static_cast<std::uint32_t>((alpha >> 6) & 3u); }
  std::uint32_t alpha_fix() const noexcept { return static_cast<std::uint32_t>((alpha >> 32) & 0xFFu); }
  std::uint32_t xyoffset_ofx() const noexcept { return static_cast<std::uint32_t>(xyoffset & 0xFFFFu); }
  std::uint32_t xyoffset_ofy() const noexcept { return static_cast<std::uint32_t>((xyoffset >> 32) & 0xFFFFu); }
};

// One vertex of a decoded primitive batch. Both the latching kicks (XYZF3/XYZ3) and the drawing kicks (XYZF2/XYZ2) are
// kept: a sprite is two kicks (the first latches the top-left corner and its UV, the second draws the bottom-right), so
// dropping the latching ones would make every sprite a single point.
struct GsDecodeVertex {
  float x = 0.0f;  // screen pixels after the 12.4 wrap rule, relative to the XYOFFSET in force at the kick
  float y = 0.0f;
  std::uint32_t z = 0;
  std::uint8_t fog = 0;
  bool has_fog = false;
  bool drawing = true;
  float u = 0.0f;  // texel coordinates (12.4 raw / 16) when the register state carried a UV at the kick
  float v = 0.0f;
  bool has_uv = false;
};

// One primitive batch: the vertex run of one tag under one PRIM. `state` is the register file at the FIRST vertex of the
// run, which is the state the draw runs under (a PRIM or TEX0 write inside the tag starts a new batch and a new state).
struct GsDecodeBatch {
  std::uint64_t vblank = 0;      // the capture VBlank stamp (the stream kicked in this interval)
  std::uint32_t sequence = 0;    // the capture record sequence the tag started in
  std::uint32_t pass = 0;        // always kPassUnknown in the S1 capture; kept for a later stream marker
  std::uint8_t path = 0;
  std::uint32_t source_pc = 0;
  std::uint32_t source_ra = 0;
  std::uint64_t last_job = 0;
  GsDecodeState state;
  std::uint32_t vertices = 0;
  std::uint32_t drawing_vertices = 0;
  std::vector<GsDecodeVertex> verts;
  float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
  bool full_screen = false;  // the measured S2 rule: width >= 600 and height >= 400 px
};

// One IMAGE upload: the transfer registers in force plus the payload size the capture carries. `data` is the payload
// the capture delivered for a host->local transfer (the S3 texture source); it is empty for a direction that carries
// no host data or when the packet was split and the capture did not deliver it.
struct GsDecodeImageUpload {
  std::uint64_t vblank = 0;
  std::uint32_t sequence = 0;
  std::uint8_t path = 0;
  std::uint64_t bytes = 0;
  std::uint32_t qwords = 0;
  std::uint32_t dbp = 0, dbw = 0, dpsm = 0;
  std::uint32_t sbp = 0, sbw = 0, spsm = 0;
  std::uint32_t dsax = 0, dsay = 0, rrw = 0, rrh = 0;
  std::uint32_t trxdir = 0;  // 0 host->local, 1 local->host, 2 local->local, 3 none
  bool has_transfer_state = false;  // BITBLTBUF/TRXREG were written before the IMAGE tag
  std::vector<std::uint8_t> data;   // host->local payload, in capture order
};

// The per-frame census row. `scene_frame = capture_vblank - 1` is the S1 phase relation (see the file comment).
struct GsFrameCensus {
  std::uint64_t capture_vblank = 0;
  std::uint64_t scene_frame = 0;
  std::uint64_t gif_records[4] = {};  // indexed by path
  std::uint64_t gif_bytes[4] = {};
  std::uint64_t packets = 0;
  std::uint64_t packed_tags = 0, reglist_tags = 0, image_tags = 0, disabled_tags = 0;
  std::uint64_t register_packets = 0;  // tags whose payload is register writes only
  std::uint64_t primitive_packets = 0;  // tags that carry vertex kicks
  std::uint64_t empty_packets = 0;
  std::uint64_t ad_writes = 0;
  std::uint64_t register_writes = 0;  // PACKED/REGLIST direct descriptor writes + A+D non-vertex writes
  std::uint64_t prim_kicks = 0;
  std::uint64_t draws = 0;            // batches with at least one drawing vertex
  std::uint64_t batches = 0;          // every vertex run
  std::uint64_t vertices = 0;
  std::uint64_t drawing_vertices = 0;
  std::uint64_t full_screen_batches = 0;
  std::uint64_t image_uploads = 0;
  std::uint64_t image_bytes = 0;
  std::uint64_t signals = 0;
};

struct GsStreamCensus {
  std::uint64_t records = 0;
  std::uint64_t gif_records[4] = {};
  std::uint64_t gif_bytes[4] = {};
  std::uint64_t packets = 0;
  std::uint64_t packed_tags = 0, reglist_tags = 0, image_tags = 0, disabled_tags = 0;
  std::uint64_t register_packets = 0, primitive_packets = 0, empty_packets = 0;
  std::uint64_t ad_writes = 0;
  std::uint64_t register_writes = 0;
  std::uint64_t prim_kicks = 0;
  std::uint64_t batches = 0, draws = 0, vertices = 0, drawing_vertices = 0, full_screen_batches = 0;
  std::uint64_t image_uploads = 0, image_bytes = 0;
  std::uint64_t signals = 0;
  // Named refusals. Every one is a packet or record the walker could not finish; no packet is skipped silently.
  std::uint64_t truncated_packets = 0;        // a tag or its data runs past the end of the capture
  std::uint64_t unknown_format_packets = 0;   // GIF format 3 (disabled)
  std::uint64_t unsupported_reglist_descriptor = 0;  // REGLIST descriptor 0xE/0xF is a no-op in the GS; counted
  std::uint64_t image_without_transfer_state = 0;    // an IMAGE tag with no BITBLTBUF/TRXREG before it
  std::uint64_t vertices_without_prim = 0;           // a kick before any PRIM write (not drawable)
  std::uint64_t split_packets = 0;                   // packets whose tag/data crossed a capture record (handled)
  std::array<std::uint64_t, 128> ad_address_hist = {};
  std::map<std::uint64_t, GsFrameCensus> frames;  // keyed by capture VBlank
};

// Walks every GIF record in `records` (the reader's table over `data`) and fills the census plus the decoded batches and
// uploads. Returns false with `error` only for a structurally invalid file the reader accepted but the walker cannot
// trust (a record table mismatch); a malformed packet inside the stream is a named refusal, not a file error.
bool census_gs_stream(const std::vector<std::uint8_t> &data, const GsStreamFileHeader &header,
                      const std::vector<GsStreamFileRecord> &records, GsStreamCensus &census,
                      std::vector<GsDecodeBatch> &batches, std::vector<GsDecodeImageUpload> &uploads,
                      std::string &error);

// The measured wet-road composite chain of one attributed scene frame (S2). The S1 phase relation maps scene frame V to
// the records stamped V + 1. The recogniser pins the exact state signature the S2 measurement found and refuses (with a
// named reason and counts) when it is not unique; it never guesses a different draw.
struct GsWetCompositeInfo {
  bool found = false;
  std::string refusal;  // named reason when found is false
  const GsDecodeBatch *alpha_clear = nullptr;    // the full-screen alpha clear (seq 920 of the capture)
  const GsDecodeBatch *composite = nullptr;      // the additive full-screen sphere-map composite (seq 934)
  const GsDecodeBatch *post_darken = nullptr;    // the captured full-screen multiply by FIX 127 (seq 936)
  const GsDecodeBatch *post_brighten = nullptr;  // the captured full-screen multiply by FIX 255 (seq 964)
  std::uint64_t unrecognised_full_screen = 0;    // full-screen batches in the stream that are not one of the four
  std::uint64_t scene_vblank = 0;
  std::uint64_t stream_vblank = 0;
};

// Finds the chain in `batches` for scene frame `scene_vblank` using the V -> V+1 phase relation.
bool find_wet_composite(const std::vector<GsDecodeBatch> &batches, std::uint64_t scene_vblank,
                        GsWetCompositeInfo &info);

// Human-readable name of a packet class and a frame census line, for the tool and the evidence.
const char *gs_batch_class_name(const GsDecodeBatch &batch) noexcept;
std::string gs_batch_state_line(const GsDecodeBatch &batch);

}  // namespace gt4::coregs3
