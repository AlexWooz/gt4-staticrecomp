// TASK-090 CoreGS 3.0: the measured car-pass texture sampler of the offline reference renderer.
//
// This unit is the sampling subset the car-pass study measured (rounds 15-16, TASK-090 texture work), written down
// explicitly and shared by the offline rasteriser and its fixture. It is Vulkan-free and decoder-free: the caller
// supplies an already decoded RGBA8 texture plus the TEX0/TEX1/CLAMP record fields and (when the tap is available) the
// per-item TEST/ALPHA/FRAME/ZBUF state. The measured subset:
//
//   formats    T4, T8 and CT32 (the decoded RGBA8 is the same bytes the native decoder produced; the palette and the
//              swizzle are the decoder's business, not the sampler's);
//   function   MODULATE only: rgb = min(255, (T * C) >> 7) per channel and a = min(255, (Ta * Ca) >> 7) when TCC = 1,
//              else a = Ca (TCC = 0). The 8-bit integer form is the GS one (0x80 / T is one, so a 0..255 vertex colour
//              may double a channel and the result saturates);
//   mips       level 0 only. TEX1.MXL must be 0; MMAG and MMIN must agree (the measured records carry 156 linear/linear
//              and 13 nearest/nearest, never a mixed pair), because the LOD that would choose between them is not
//              computed here;
//   filtering  nearest (floor of the 10.4 coordinate) or bilinear (the spec's (u - 0.5) floor base, 4-bit weights and
//              a >> 8 sum), exactly the arithmetic of native/hw_gs/pixel/hw_gs_pixel.cpp;
//   wrapping   REPEAT, CLAMP, REGION_CLAMP and REGION_REPEAT with the chapter 01.1 formulas. The night car pass measured
//              region clamp with the full-size region (MINU 0, MAXU 2^TW-1, i.e. a clamp to the texture) or repeat; a
//              wrapped texel outside the rectangle the decoder produced is refused, never zero-filled;
//   alpha test TEST.ATE with the eight ATST comparisons and TEST.AREF; only AFAIL = KEEP is reproduced (a failing
//              fragment writes no colour and no depth). DATE and any other AFAIL are refused, counted, not defaulted;
//   blending   the measured equations, normal `(Cs - Cd) * As + Cd` and additive `(Cs - 0) * Ad + Cd`, evaluated with
//              the GS integer form `(((A - B) * C) >> 7) + D` and clamped (COLCLAMP = 1, the default and the measured
//              car state). The written alpha is the fragment's alpha. Another ALPHA equation is refused, not guessed;
//   FBMSK      FRAME.FBMSK merge, `new = (old & FBMSK) | (computed & ~FBMSK)` per byte lane (bit 1 keeps the old bit).
//
// A record that does not match the subset is refused with a status that names why; a state that cannot be verified is a
// counted fallback in the renderer, never a silent default.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace gt4::coregs3 {

// One decoded texture in the sampler's terms. `rgba` points into the frame table's cache (width * height * 4 bytes,
// row-major, top row first) and stays valid for the table's lifetime.
struct SampledTexture {
  std::uint32_t psm = 0;  // TEX0.PSM: T4 0x14, T8 0x13, CT32 0x00
  std::uint32_t tw = 0;   // TEX0 log2 width
  std::uint32_t th = 0;   // TEX0 log2 height
  std::uint32_t tcc = 1;  // TEX0.TCC: texture alpha participates in MODULATE
  std::uint32_t tfx = 0;  // TEX0.TFX: MODULATE is 0 and the only measured function
  std::uint32_t mxl = 0;  // TEX1.MXL: level 0 only, so this must be 0
  // TASK-090 track M1: the record's TEX1.MXL was above 0 and the world table decoded level 0 as a declared fallback. The
  // sampler still samples level 0 (mxl is stored as 0); the flag lets the report count the fallback.
  bool level0_mip_fallback = false;
  std::uint32_t mmag = 0; // TEX1.MMAG: 0 nearest, 1 linear
  std::uint32_t mmin = 0; // TEX1.MMIN: 0 nearest, 1 linear
  std::uint32_t wms = 0;  // CLAMP.WMS/WMT: 0 repeat, 1 clamp, 2 region clamp, 3 region repeat
  std::uint32_t wmt = 0;
  std::uint32_t minu = 0, maxu = 0, minv = 0, maxv = 0;
  std::uint32_t width = 0, height = 0;  // the rectangle the decoder produced
  std::uint32_t x0 = 0, y0 = 0;         // its top-left in texture coordinates
  std::uint32_t cache_slot = 0;         // the frame cache slot the bytes came from (reporting only)
  const std::uint8_t *rgba = nullptr;   // width * height * 4
  // TASK-090 track textures: where this sampler's state came from. `from_state` is true when the TEX0/TEX1/CLAMP fields
  // are the register words the VU1 tap measured at the draw; false when the 40-byte PGLUtexture record was the source.
  // `palette_via_tex2` marks an indexed texture whose palette bank was loaded by a TEX2 write (TEX0.CLD = 0).
  bool from_state = false;
  bool palette_via_tex2 = false;
  std::uint64_t source_tex0 = 0;   // the TEX0/TEX1/CLAMP words the state fields were decoded from
  std::uint64_t source_tex1 = 0;
  std::uint64_t source_clamp = 0;
};

// The Format G content key fields (native/hw_gs/replace/include/hw_gs_replace/keys.hpp): the frame table decodes one
// texture per key per frame, so two records with the same bytes and palette share the decoded RGBA8.
struct TextureKey {
  std::uint64_t texture_hash = 0;
  std::uint64_t palette_hash = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  friend bool operator==(const TextureKey &, const TextureKey &) = default;
};

// Per-item GS pixel state. The tap's A+D state records provide TEST/ALPHA/FRAME/ZBUF/PRIM; a field the tap did not
// carry uses the measured car-pass default below and keeps its `*_known` flag false so the report can say so.
struct SampleState {
  bool from_tap = false;
  bool test_known = false;
  bool alpha_known = false;
  bool frame_known = false;
  bool zbuf_known = false;
  bool prim_known = false;
  std::uint64_t test = 0;   // TEST, canonical 64-bit layout
  std::uint64_t alpha = 0;  // ALPHA, canonical 64-bit layout
  std::uint32_t fbmsk = 0;  // FRAME.FBMSK
  std::uint32_t zmsk = 0;   // ZBUF.ZMSK
  std::uint32_t prim = 0;   // PRIM in force at the item's first tapped vertex
  std::uint32_t fog_colour = 0;  // FOGCOL (R in bits 0-7, G 8-15, B 16-23); 0 = black, the measured reflection pass
  // TASK-090 track M1: XYOFFSET_1/2 (0x18/0x19) in force at the item's first tapped vertex, when the tap carried it.
  // The main-view course items measure 0x6C08/0x7208; the mirror-view items 0x7C08/0x7808 and 0x7C08/0x7C08. The tool
  // uses this to select the main view and to count the exclusions; the renderer never needs it (the clip-space path is
  // resolution independent).
  std::uint64_t xyoffset = 0;
  bool xyoffset_known = false;
  // TASK-090 track M1: the state was not paired to a tapped vertex record but taken from the course pass's journal
  // (the pass-begin GS shadow captured at the K7 marker). It is measured pass state, not a per-item record; the tool
  // counts it so a reader can tell a paired item from a pass-defaulted one.
  bool world_pass_default = false;
  // TASK-090 track M1: the item's paired tapped record carried TEST = 0 (ZTST NEVER in the GS), i.e. it draws nothing.
  // The course journal TEST then replaces the field for the rest of the item; this flag keeps the zero-test exclusion
  // visible to the selection rule.
  bool world_test_zero = false;
};

// The measured car-pass default state, used when no tap is supplied or a state record never wrote the field:
//   TEST  = ATE 1, ATST GREATER (6), AREF 32, AFAIL KEEP (0), DATE 0, DATM 1, ZTE 1, ZTST GEQUAL (2) = 0x0005820D;
//   ALPHA = (Cs - Cd) * As + Cd, FIX 0 = 0x00000044 (the normal blend every measured car record carries);
//   FRAME.FBMSK = 0 (write all lanes); ZBUF.ZMSK = 0 (write depth); PRIM = 0x3C (TME and FGE on, ABE off: the measured
//   base car draw's tag PRIM; the reflection pass's own 0x7C is attached from its tapped record); FOGCOL = black. The
//   flags stay false: a caller can tell a defaulted field from a measured one.
SampleState measured_sample_state() noexcept;

// One item's binding in a frame's texture table. A null `texture` means "keep the lit/base colour" (the body's texgen
// reflection, an item without a texture record, a refused decode, or a state the sampler does not reproduce).
struct TextureBinding {
  const SampledTexture *texture = nullptr;
  SampleState state;
  std::uint32_t texture_record = 0xFFFFFFFFu;  // kNoRecord when the item has none
  // TASK-090 reflection milestone: the item is a body/paint reflection pass. Its texture is the CT32 environment map
  // sampled with the pinned texgen S,T (coregs3_reflection.hpp), and the state is the tapped PRIM 0x7C pass (additive
  // blend, destination alpha test, no depth write). `vertex_fog` is the per-vertex fog the tapped reflection record
  // carried, one byte per item vertex in mesh order; empty means the record carried no fog (a counted fallback).
  bool reflection = false;
  std::vector<std::uint8_t> vertex_fog;
};

// The frame's per-item bindings, one inner vector per captured draw (sized to item_count) keyed by the draw's sequence,
// so a tool that filters the draw list keeps the table valid. The table owns the decoded bytes and the SampledTexture
// records the bindings point at; a deque keeps those addresses stable while the frame is built.
struct FrameTextureTable {
  std::vector<std::uint32_t> sequence;  // parallel to draws
  std::vector<std::vector<TextureBinding>> draws;
  std::deque<SampledTexture> textures;
  std::vector<std::vector<std::uint8_t>> payloads;  // decoded RGBA8, one per Format G cache slot

  const std::vector<TextureBinding> *for_draw(std::uint32_t draw_sequence) const noexcept {
    for (std::size_t index = 0; index < sequence.size(); ++index)
      if (sequence[index] == draw_sequence) return &draws[index];
    return nullptr;
  }

  // The bindings hold pointers into `textures` and `payloads`, so the table is movable (references into a moved deque
  // and vector stay valid) but never copyable.
  FrameTextureTable() = default;
  FrameTextureTable(const FrameTextureTable &) = delete;
  FrameTextureTable &operator=(const FrameTextureTable &) = delete;
  FrameTextureTable(FrameTextureTable &&) = default;
  FrameTextureTable &operator=(FrameTextureTable &&) = default;
};

// Why a record or state is outside the measured subset. `ok` is the only success.
enum class SampleStatus : std::uint32_t {
  ok = 0,
  refused_psm = 1,     // not T4, T8 or CT32
  refused_tfx = 2,     // TEX0.TFX is not MODULATE
  refused_filter = 3,  // MMAG != MMIN (the LOD that would pick between them is not computed)
  refused_region = 4,  // a wrapped texel is outside the decoded rectangle
  refused_tcc = 5,     // reserved (TCC is implemented); kept so the numbering never shifts
  refused_mip = 6,     // TEX1.MXL is not 0: mip levels are not decoded or sampled
};
const char *sample_status_name(SampleStatus status) noexcept;

// Samples one fragment. `vertex` is the lit/vertex colour at the pixel (8-bit per channel), `out` the MODULATE
// result. Returns ok and writes `out`, or a refusal status and leaves `out` untouched.
SampleStatus sample_texture(const SampledTexture &texture, float u, float v,
                            const std::array<std::uint8_t, 4> &vertex, std::array<std::uint8_t, 4> &out) noexcept;

// The texel coordinate of a normalised u/v, in the GS 10.4 fixed point: floor(uv * 2^log2_size * 16), saturated to
// +/-1e9 so the integer wrap below stays defined (the same conversion as ToTexel104 in coregs3/cpuref).
std::int32_t texel_coordinate_10_4(float uv, std::uint32_t log2_size) noexcept;

// The chapter 01.1 wrap formula for one axis. `minimum` is MINU/MINV, `maximum` MAXU/MAXV.
std::int32_t wrap_texel(std::int32_t texel, std::uint32_t log2_size, std::uint32_t mode, std::uint32_t minimum,
                        std::uint32_t maximum) noexcept;

// The alpha test over the measured/refused states. `pass` and `fail` are the two reproducible outcomes; the two refusal
// values are the deferred DATE and the unmeasured AFAIL modes.
enum class AlphaTestResult : std::uint32_t { pass = 0, fail = 1, refused_date = 2, refused_fail_mode = 3 };
AlphaTestResult alpha_test(std::uint64_t test, std::uint32_t fragment_alpha) noexcept;
// The ATE/ATST/AREF/AFAIL part alone, for a caller that implements the destination alpha test (DATE) itself. The
// reflection pass uses this: it evaluates DATE against the framebuffer alpha at the pixel.
AlphaTestResult alpha_test_ignore_date(std::uint64_t test, std::uint32_t fragment_alpha) noexcept;
// TASK-090 track M1: the ATE/ATST/AREF comparison alone (AFAIL ignored), for a caller that implements all four AFAIL
// write modes itself. Returns pass or fail only.
AlphaTestResult alpha_test_ate_only(std::uint64_t test, std::uint32_t fragment_alpha) noexcept;
const char *alpha_test_result_name(AlphaTestResult result) noexcept;

// Blend families the car pass measured. `normal` = A Cs, B Cd, C As, D Cd; `additive` = A Cs, B 0, C Ad, D Cd.
enum class BlendFamily : std::uint32_t { normal = 0, additive = 1, other = 2 };
BlendFamily blend_family(std::uint64_t alpha_register) noexcept;
const char *blend_family_name(BlendFamily family) noexcept;

// Decodes one ALPHA register write into its (A, B, C, D) selectors and FIX, so a caller can report the equation.
struct BlendEquation {
  std::uint32_t a = 0;  // 0 Cs, 1 Cd, 2/3 zero
  std::uint32_t b = 0;
  std::uint32_t c = 0;  // 0 As, 1 Ad, 2/3 FIX
  std::uint32_t d = 0;  // 0 Cs, 1 Cd, 2/3 zero
  std::uint32_t fix = 0;
};
BlendEquation decode_blend_equation(std::uint64_t alpha_register) noexcept;

// Applies one measured blend equation per RGB channel: `(((A - B) * C) >> 7) + D`, clamped to 0..255. `out` receives
// four bytes: the blended RGB and the fragment alpha (As), which is what the GS writes to the colour alpha lane.
void blend_fragment(const BlendEquation &equation, const std::array<std::uint8_t, 4> &source,
                    const std::array<std::uint8_t, 4> &destination, std::array<std::uint8_t, 4> &out) noexcept;

// The static per-item checks the renderer runs once before rasterising: a binding whose state is outside the measured
// subset keeps the lit colour and is counted with a reason.
enum class BindingStatus : std::uint32_t {
  ok = 0,
  no_texture = 1,        // the table has no texture for this item (not a refusal)
  refused_psm = 2,
  refused_tfx = 3,
  refused_filter = 4,
  refused_blend = 5,     // ABE on with an ALPHA equation outside normal/additive
  refused_date = 6,      // DATE on: destination alpha test is deferred
  refused_alpha_fail = 7,  // ATE on with an AFAIL other than KEEP
  refused_mip = 8,       // TEX1.MXL is not 0
};
const char *binding_status_name(BindingStatus status) noexcept;
BindingStatus check_binding(const TextureBinding &binding) noexcept;

// TASK-090 track M1: the world path's static checks. It differs from check_binding in two measured ways: the destination
// alpha test (DATE) is implemented by the world rasteriser, so a DATE state is accepted; and any ALPHA equation the
// selector model can evaluate is accepted (the sky's tapped `(Cs-Cd)*As+Cs` is outside the car pass's normal/additive
// families). The texture, TFX, filter and mip subset is the same.
BindingStatus check_world_binding(const TextureBinding &binding) noexcept;

// Decodes FRAME.FBMSK from a FRAME register write (bits 32-63), for the table builder and the tests.
std::uint32_t frame_fbmsk(std::uint64_t frame_register) noexcept;
// Decodes ZBUF.ZMSK from a ZBUF register write (bit 32).
std::uint32_t zbuf_zmsk(std::uint64_t zbuf_register) noexcept;
// True when PRIM.ABE (bit 6; native/hw_gs/frontend/hw_gs_types.hpp primbit::kAbe) enables blending.
bool prim_blend_enabled(std::uint32_t prim) noexcept;
// True when PRIM.TME (bit 4) enables texture mapping.
bool prim_texture_enabled(std::uint32_t prim) noexcept;

}  // namespace gt4::coregs3
