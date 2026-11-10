// TASK-090 CoreGS 3.0: the measured VU1 lighting programs, shared by the offline reference renderer
// (native/coregs3/coregs3_render.cpp), the live `present` layer (native/hw_gs/coregs3/hw_coregs3_present.cpp) and the
// evidence tools.
//
// The car pass runs one VU1 microprogram per item. The item's ITOP word selects it (the raw VIF ITOP immediate is in
// 8-byte units, so the dispatch address is `(run_itop & 0x3FF) * 8`), and the item's own light block is the state the
// program reads at TOPS+0xC9. Round 13 mapped the dispatches; round 14 measured the formulas against the real AOT
// fragments and the r13 tap (docs/COREGS3-HOOKS.md sections 12-13, evidence under build/analysis-lighting/r14/). The
// per-item block and selector come from `.gt4scene` version 4 (coregs3_scene.hpp, CapturedItem).
//
// The body program and the seven measured families, their constants and their inputs:
//
//   body_paint        ITOP 0x1058, six-qword block 0x6C06C0C9 at 0xC9..0xCE. The existing formula (round 12):
//                     P = raw V3-16 position after ITOF0, N = V3-8 normal at TOPS+0x40 / 127, B = V4-8 colour at
//                     TOPS+0x80; D = P - LP, t = min(dot(D,N), 0) / |D|, out = FTOI0(B * (C3 + C4 * t)) per lane.
//
//   nine_qword_first  ITOP 0x0B08, nine-qword block 0x6C09C0C9 at 0xC9..0xD1. N = V3-8 normal at TOPS+0x80 / 127.
//                     q0..q2 are the three rows of the light matrix M: direction j is (q0[j], q1[j], q2[j]); with
//                     L_j = max(dot(direction_j, N), 0):
//                       out = FTOI0(clamp(q3 + q4*L0 + q5*L1 + q6*L2 + q7*L3, 0, q8)) per lane, q3 = 0xCC .. q8 = 0xD1.
//                     Lane 3 uses q8.w (128 in every captured block), so alpha is exact.
//
//   nine_qword_pow8   ITOP 0x0250, loop 0x0290, same nine-qword block and normal, but lanes 2 and 3 of the light
//                     vector are raised to the eighth power (three squarings) before the coefficients:
//                       out = FTOI0(clamp(q3 + q4*L0 + q5*L1 + q6*L2^8 + q7*L3^8, 0, q8)).
//                     The task brief grouped 0x0250 with the first-power family; the real AOT fragment disproves that:
//                     with N = (127,127,115)/127 and q4..q7 = 10, first-power gives 29, power-8 gives 24 and power-16
//                     gives 22, and the fragment writes 24 for dispatch 0x0250 (probe evidence in the fixture and
//                     docs/COREGS3-HOOKS.md section 14).
//
//   nine_qword_pow16  ITOP 0x03D8 (V3-16 positions, V3-8 normal at 0x80 through the 0x1878 prologue) and the direct
//                     ITOP 0x0418 (no prologue: V3-32 positions and V3-32 unit normals at TOPS+0x80). Same block:
//                       out = FTOI0(clamp(q3 + q4*L0 + q5*L1 + q6*L2^16 + q7*L3^16, 0, q8)),
//                     with L2^16 / L3^16 built by four squarings.
//
//   three_qword       ITOP 0x14D8, three-qword block 0x6C03C0C9 at 0xC9..0xCB: gain q0, clamp q1, bias q2. C is the
//                     item's V4-8 colour at TOPS+0x80 after ITOF0 (0..255):
//                       out = FTOI0(clamp(q2 + q0 * C, 0, q1)) per lane, lane 3 included.
//
//   six_qword_no_colour ITOP 0x0C68, six-qword block 0x6C06C0C9 at 0xC9..0xCE. P = raw V3-16 position after ITOF0,
//                     N = V3-8 normal at TOPS+0x80 / 127, B = q3 (0xCC):
//                       D = P - B, R = D - 2N(N.D), q = RSQRT(|D|^2), t = R * q (SIGNED),
//                       L = q0*t0 + q1*t1 + q2*t2, L = max(L, 0),
//                       out = FTOI0(q4 * L.x^8) per lane (three squarings), alpha = 128.
//                     The `max` is applied to each component of L after the weighted sum, not to t: with a negative
//                     t0 the fragment's scalar is lower by exactly q0.x*t0 (measured on the probe harness, see the
//                     fixture). The output uses L.x only (the fragment broadcasts lane 0 before the squarings).
//
//   passthrough       ITOP 0x0080, no block: the V4-8 colour at TOPS+0x80 is emitted unchanged, alpha included.
//
// Item-level fallbacks are explicit and counted, never guessed: an invalid run pair, an unmeasured selector, a missing
// block (`light_source == none`), a block tracked after a chain discontinuity (`carried_uncertain`) and a block whose
// qword count/command is not the form that program reads all reject the whole item and keep its base colour. A carried
// block is accepted (it is the VU1's carried state, tracked in VIF command order); its command word may be zero when
// the capture did not keep it, so a carried block is validated by its qword count and, when present, its command. The
// passthrough program reads no block, so its item's light_source does not gate it.
//
// Evidence (round 11/12/14, TASKS.md TASK-090): the body formula reproduces the tapped RGBAQ on 3,822 unambiguous body
// vertices with max 1 level and alpha exact; round 14's validator reproduces every family with max 1 level and alpha
// exact where the item's own block is captured. The rms/max of the implementation against the v4 capture and tap are in
// docs/COREGS3-HOOKS.md section 14.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

namespace gt4::coregs3 {

// Values of the six-qword body/paint block, as the capture records them. The command is the tag's VIF UNPACK word:
// V4-32 (0x6C), NUM = 6 qwords, destination TOPS+0xC9. The qword count is part of the variant: a file that carries a
// different count is a different upload, not this one.
inline constexpr std::uint32_t kBodyPaintLightBlockCommand = 0x6C06C0C9u;
inline constexpr std::uint32_t kBodyPaintLightBlockQwords = 6u;
// The nine-qword glass/lamp form (V4-32, NUM = 9) and the three-qword gain/clamp/bias form (V4-32, NUM = 3).
inline constexpr std::uint32_t kNineQwordLightBlockCommand = 0x6C09C0C9u;
inline constexpr std::uint32_t kNineQwordLightBlockQwords = 9u;
inline constexpr std::uint32_t kThreeQwordLightBlockCommand = 0x6C03C0C9u;
inline constexpr std::uint32_t kThreeQwordLightBlockQwords = 3u;

// The decoded body/paint light block. Raw float values, exactly as the VU1 received them.
struct LightBlock {
  std::array<float, 4> matrix[3]{};  // M, qwords 0xC9..0xCB; not part of the lit colour
  std::array<float, 4> position{};   // LP, qword 0xCC
  std::array<float, 4> c3{};         // C3, qword 0xCD
  std::array<float, 4> c4{};         // C4, qword 0xCE
};

enum class LightBlockStatus : std::uint8_t {
  ok = 0,
  missing,          // the draw carried no light block upload (light_block_qwords == 0)
  unknown_variant,  // a block is present but it is not the measured six-qword body/paint form
  not_finite,       // one of the qword values is NaN or infinite
};
const char *light_block_status_name(LightBlockStatus status) noexcept;

// Reads the per-draw light block of one draw. Kept for the round 12 API and its fixture; the renderers use the per-item
// state below. Returns ok and fills `block` only for the measured body/paint variant; for every other status `block`
// is left untouched and the caller must fall back.
LightBlockStatus decode_light_block(const struct CapturedDraw &draw, LightBlock &block) noexcept;

// Shades one vertex with the captured body/paint block. `vertex` must carry the V3-8 normal at TOPS 0x40 and the raw
// V3-16 position (MeshVertex::normal / raw_position); `rgba` receives the lit colour. Returns false and leaves `rgba`
// untouched when the vertex cannot be shaded with this block: no normal, no raw position, a zero/non-finite (P - LP),
// or a non-finite result. The caller then keeps the base colour and counts the fallback.
bool shade_vertex(const LightBlock &block, const struct MeshVertex &vertex,
                  std::array<std::uint8_t, 4> &rgba) noexcept;

// The same formula without a MeshVertex, for tools and fixtures. `normal` is the raw int8 triplet, `raw_position` the
// signed integer the VU1 converts with ITOF0, `base` the V4-8 colour.
bool shade_vertex(const LightBlock &block, const std::array<std::int8_t, 3> &normal,
                  const std::array<std::int32_t, 3> &raw_position, const std::array<std::uint8_t, 4> &base,
                  std::array<std::uint8_t, 4> &rgba) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// Per-item programs (format version 4)

enum class LightProgram : std::uint8_t {
  none = 0,           // no measured program: the item keeps its base colour
  body_paint,         // ITOP 0x1058, six-qword block
  nine_qword_first,   // ITOP 0x0B08, nine-qword block, first power
  nine_qword_pow8,    // ITOP 0x0250, nine-qword block, L2/L3 to the eighth power
  nine_qword_pow16,   // ITOP 0x03D8, nine-qword block, L2/L3 to the sixteenth power
  nine_qword_direct,  // ITOP 0x0418, nine-qword block, V3-32 positions and normals (no prologue)
  six_qword_no_colour,  // ITOP 0x0C68, six-qword block, Phong-style specular
  three_qword_gain,   // ITOP 0x14D8, three-qword block, gain/clamp/bias on the V4-8 colour
  passthrough,        // ITOP 0x0080, no block: the V4-8 colour is emitted unchanged
  count,
};
inline constexpr std::uint32_t kLightProgramCount = static_cast<std::uint32_t>(LightProgram::count);
const char *light_program_name(LightProgram program) noexcept;

// Maps a VU1 ITOP dispatch address (the byte address, `(run_itop & 0x3FF) * 8`) to the measured program. Unknown
// dispatches return LightProgram::none.
LightProgram light_program_from_dispatch(std::uint32_t dispatch) noexcept;
// The dispatch address of an item's run pair; 0 when the pair is not a valid ITOP/MSCAL form.
std::uint32_t item_dispatch(const CapturedItem &item) noexcept;
// The program of an item, or none when its run pair is invalid or its selector is not measured.
LightProgram light_program_of_item(const CapturedItem &item) noexcept;

// The outcome of preparing or shading one item/vertex. `ok` is the only usable state.
enum class LightStatus : std::uint8_t {
  ok = 0,
  no_program,        // the run pair is invalid or the ITOP selector is not one of the measured programs
  no_block,          // light_source == none: the block the program reads is unknown
  uncertain_block,   // light_source == carried_uncertain: the block parity could not be proven
  block_variant,     // qword count/command is not the measured form that program reads
  block_not_finite,  // a decoded qword is NaN or infinite
  missing_channel,   // the vertex lacks a channel the program reads (normal, colour or raw position)
  invalid_math,      // the fragment would divide by zero or the result is not finite
  count,
};
inline constexpr std::uint32_t kLightStatusCount = static_cast<std::uint32_t>(LightStatus::count);
const char *light_status_name(LightStatus status) noexcept;

// One item's decoded light state: the program its VU1 run executes and the nine qwords it may read at TOPS+0xC9
// (valid ones only; qwords past the program's form stay zero).
struct ItemLightState {
  LightProgram program = LightProgram::none;
  std::array<std::array<float, 4>, 9> qword{};
};

// Decodes one item's block and selector. Returns ok when the item can be shaded (for `passthrough` the block is not
// read and no qword is decoded); otherwise a fallback status and the base colour must be kept.
LightStatus decode_item_light(const CapturedItem &item, ItemLightState &state) noexcept;

// Shades one vertex with the item's decoded state. Returns ok and writes `rgba`; otherwise returns the fallback reason
// and leaves `rgba` untouched.
LightStatus shade_item_vertex(const ItemLightState &state, const MeshVertex &vertex,
                              std::array<std::uint8_t, 4> &rgba) noexcept;

// Per-family and per-status counters shared by both renderers. A draw can mix programs (the capture shows it), so the
// family draw counts answer "draws with at least one lit/fallback vertex of this family"; items and vertices are
// counted exactly once. `status` counts every mesh vertex that ended in that state.
struct LightFamilyCounts {
  std::uint64_t items = 0;
  std::uint64_t items_lit = 0;
  std::uint64_t items_fallback = 0;
  std::uint64_t vertices_lit = 0;
  std::uint64_t vertices_fallback = 0;
  std::uint64_t draws_lit = 0;
  std::uint64_t draws_fallback = 0;
};

struct LightCounters {
  std::array<LightFamilyCounts, kLightProgramCount> family{};
  std::array<std::uint64_t, kLightStatusCount> status{};
  std::uint64_t draws_lit = 0;         // draws with at least one lit vertex
  std::uint64_t draws_fallback = 0;    // draws with no lit vertex
  std::uint64_t vertices_lit = 0;
  std::uint64_t vertices_fallback = 0;
  std::uint64_t items_uploaded = 0;    // item light_source tally
  std::uint64_t items_carried = 0;
  std::uint64_t items_uncertain = 0;
  std::uint64_t items_none = 0;

  void add(const LightCounters &other) noexcept;
};

// One vertex's outcome, aligned with Mesh::vertices when the caller asks for it.
struct ShadedVertex {
  LightProgram program = LightProgram::none;
  LightStatus status = LightStatus::no_program;
};

// Shades every vertex of `mesh` with its own item's program and block, in place (MeshVertex::color). The mesh must have
// been built from `draw` by coregs3_shape (MeshVertex::item is the original draw item index). `counters` accumulates;
// `shades`, when not null, is resized to the vertex count and filled with each vertex's program/status. Vertices whose
// item or channels cannot be shaded keep their decoded base colour.
void shade_mesh_items(const CapturedDraw &draw, Mesh &mesh, LightCounters &counters,
                      std::vector<ShadedVertex> *shades = nullptr);

}  // namespace gt4::coregs3
