// TASK-090 CoreGS 3.0, phase S3: the first native HUD/2D layer, the speed element of the race panel.
//
// The capture work proved the race panel is mixed: the elements' text/digits are PATH2 sprites of the `.gt4gs` stream
// kicked one VBlank after the scene frame, while the element plates (the dark rounded backgrounds, `km/h`) are VU1
// PATH1 draws recorded in the `.gt4tap`. This unit joins both sources for one element and draws it over an offline
// world frame with the captured GS state, the captured textures and the measured 2D placement.
//
// Geometry (C, measured on the night race, streams 7398..7405):
//   * screen 640x448; the HUD ortho is the measured XYOFFSET 0x6C08/0x7208 (OFX 1728.5, OFY 1824.5) with the same
//     12.4 wrap rule the S2 walker uses;
//   * the speed element is the bottom-right plate: PATH1 quad x 550.5..612.0, y 377.5..403.6 (the
//     `remote_speed_kmph` plate), then PATH2 digit sprites x 567.75..578.25 and 577.5..588.0, y 382.5..400.5;
//   * the digits are drawn under SCISSOR 0..639 x 45..76 when only the PATH2 register file is walked (a stale write
//     of the position element). The real GS state is the merged PATH1+PATH2 order: the plate's VU1 job (a lower job id)
//     writes SCISSOR 0..639 x 0..447, so the digits survive. The panel builder therefore merges the tap's register
//     writes by job id and counts how many sprites the PATH2-only state would have culled.
//
// Textures (C): the digit atlas is T4 128x64 (TEX0 TBP0 0x3A20/0x3A00, CBP +7, CLAMP 0..127 x 0..38, CT32 CSM1
// palette, TCC 1, MODULATE, linear, MXL 0); the plate is T8 128x32 (TBP0 0x39E0, CBP +8, CLAMP 0..81 x 0..25). Both
// are uploaded by the frame's own PATH3 IMAGE packets and are decoded from the captured GS local memory (the
// `.gt4vram` sidecar), never guessed from the disc. TEX0.CLD is 0 because the palette bank is loaded by the TEX2 write
// that precedes the draw; the sidecar holds that bank at CBP, so the synthesized record sets CLD = 1 and the decoder
// reads exactly the bank the draw samples (documented in the report).
//
// Drawing model (measured): sprite/tri-strip coverage by pixel centres (the CoreGS 2.0 rule), TFX MODULATE
// (Cs = Ct*Cv >> 7), the ALPHA register's (Cs-Cd)*As+Cd blend, ATE off for this element, the display buffer's CT16
// destination model (5-bit values stored << 3, the reference's own expansion). Every refusal is named and counted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "coregs3_gsdecode.hpp"
#include "coregs3_tap_file.hpp"
#include "coregs3_texture.hpp"

namespace gt4::coregs3 {

// The elements S3 implements. `speed` is the measured night B-Spec speed plate and digits; `tach` is the A-Spec
// tachometer (the right gauge's dial plate and its untextured triangle-strip needle); `all` is the census-driven mode
// that draws every panel entry the raster supports.
enum class HudElement : std::uint32_t { speed = 0, tach = 1, all = 2 };
bool parse_hud_element(const std::string &name, HudElement &element);
const char *hud_element_name(HudElement element) noexcept;

// The register subset a HUD draw uses, in the canonical 64-bit layouts (GsDecodeState for PATH2, the tap's A+D words
// for PATH1).
struct HudDrawState {
  std::uint32_t prim = 0;
  std::uint64_t frame = 0;  // FRAME_1/2
  std::uint64_t zbuf = 0;
  std::uint64_t test = 0;
  std::uint64_t alpha = 0;
  std::uint64_t scissor = 0;  // effective (merged) scissor
  std::uint64_t xyoffset = 0;
  std::uint64_t tex0 = 0;
  std::uint64_t tex1 = 0;
  std::uint64_t clamp = 0;
  std::uint32_t rgba = 0x80808080u;  // vertex colour (R | G<<8 | B<<16 | A<<24), flat shade

  std::uint32_t frame_fbp() const noexcept { return static_cast<std::uint32_t>(frame & 0x1FFu); }
  std::uint32_t frame_fbw() const noexcept { return static_cast<std::uint32_t>((frame >> 16) & 0x3Fu); }
  std::uint32_t frame_psm() const noexcept { return static_cast<std::uint32_t>((frame >> 24) & 0x3Fu); }
  std::uint32_t tex0_tbp0() const noexcept { return static_cast<std::uint32_t>(tex0 & 0x3FFFu); }
  std::uint32_t tex0_tbw() const noexcept { return static_cast<std::uint32_t>((tex0 >> 14) & 0x3Fu); }
  std::uint32_t tex0_psm() const noexcept { return static_cast<std::uint32_t>((tex0 >> 20) & 0x3Fu); }
  std::uint32_t tex0_tw() const noexcept { return static_cast<std::uint32_t>((tex0 >> 26) & 0xFu); }
  std::uint32_t tex0_th() const noexcept { return static_cast<std::uint32_t>((tex0 >> 30) & 0xFu); }
  std::uint32_t tex0_tcc() const noexcept { return static_cast<std::uint32_t>((tex0 >> 34) & 1u); }
  std::uint32_t tex0_tfx() const noexcept { return static_cast<std::uint32_t>((tex0 >> 35) & 3u); }
  std::uint32_t tex0_cbp() const noexcept { return static_cast<std::uint32_t>((tex0 >> 37) & 0x3FFFu); }
  std::uint32_t tex0_cpsm() const noexcept { return static_cast<std::uint32_t>((tex0 >> 51) & 0xFu); }
  std::uint32_t tex0_csm() const noexcept { return static_cast<std::uint32_t>((tex0 >> 55) & 1u); }
  std::uint32_t tex0_csa() const noexcept { return static_cast<std::uint32_t>((tex0 >> 56) & 0x1Fu); }
  std::uint32_t tex0_cld() const noexcept { return static_cast<std::uint32_t>((tex0 >> 61) & 0x7u); }
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
  std::uint32_t alpha_a() const noexcept { return static_cast<std::uint32_t>(alpha & 3u); }
  std::uint32_t alpha_b() const noexcept { return static_cast<std::uint32_t>((alpha >> 2) & 3u); }
  std::uint32_t alpha_c() const noexcept { return static_cast<std::uint32_t>((alpha >> 4) & 3u); }
  std::uint32_t alpha_d() const noexcept { return static_cast<std::uint32_t>((alpha >> 6) & 3u); }
  std::uint32_t alpha_fix() const noexcept { return static_cast<std::uint32_t>((alpha >> 32) & 0xFFu); }
};

// One vertex of a panel draw. `x`/`y` are screen pixels after the measured 12.4 wrap rule; `u`/`v` are texel
// coordinates for PATH2 sprites and normalised ST (0..1 across the texture's TW/TH) for PATH1 plates.
struct HudVertex {
  float x = 0.0f;
  float y = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
  std::uint32_t rgba = 0x80808080u;
  bool drawing = true;
};

struct HudDraw {
  bool path1 = false;  // a VU1 tap draw (the element plate) instead of a PATH2 sprite
  std::uint64_t job = 0;
  std::uint32_t sequence = 0;  // PATH2: the batch's capture sequence; PATH1: the tap record ordinal
  HudDrawState state;
  std::uint32_t vertices = 0;
  HudVertex verts[8];
  bool normalized_uv = false;  // PATH1 plates carry ST, PATH2 sprites texel UV
  bool triangle_strip = false;  // PATH1: an axis-aligned 4-vertex tri-strip quad
  // TASK-090 HUD A-Spec: an untextured Gouraud triangle strip (the tachometer/speedometer needles). The vertex
  // colours interpolate like PRIM.IIP, ATE/blend use the captured state, and `drawing == false` vertices (ADC) break
  // the strip as the GS does. The raster covers pixel centres inside each triangle with the top-left fill rule, so the
  // shared edge of the two triangles is written once.
  bool untextured_strip = false;
  float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
};

struct HudPanelCounters {
  std::uint64_t path2_batches = 0;           // batches in the panel range of the attributed stream
  std::uint64_t path2_selected = 0;          // the requested element's sprites
  std::uint64_t path2_other = 0;             // panel batches that belong to another element
  std::uint64_t path2_stream_scissor_culled = 0;  // selected sprites the PATH2-only scissor would have dropped
  std::uint64_t path1_draws = 0;             // tap draws into the display buffer
  std::uint64_t path1_selected = 0;          // the plate quad(s) of the element
  std::uint64_t path1_other = 0;
  std::uint64_t path1_needles = 0;           // untextured triangle-strip gauge needles selected
  std::uint64_t plate_quads = 0;
  std::uint64_t refusals = 0;
  std::map<std::string, std::uint64_t> reasons;  // every named refusal/counter

  void count(const std::string &reason) { reasons[reason] += 1; }
};

struct HudPanel {
  std::uint64_t scene_vblank = 0;
  std::uint64_t stream_vblank = 0;
  HudElement element = HudElement::speed;
  std::vector<HudDraw> draws;
  HudPanelCounters counters;
  std::string summary;  // one human-readable line per selected draw (for the report)
};

// Builds the panel layer for scene frame `scene_vblank` from the decoded `.gt4gs` batches (all frames) and the tap
// records. The stream is the S1 phase relation's `scene_vblank + 1`. A missing tap or no plate is a named refusal in
// `panel.counters`, not a silent skip; `error` is set only for a structural problem (malformed tap record).
bool build_hud_panel(const std::vector<GsDecodeBatch> &batches, const std::vector<std::uint8_t> &tap_data,
                     const std::vector<TapFileRecord> &tap_records, std::uint64_t scene_vblank, HudElement element,
                     HudPanel &panel, std::string &error);

// ---- The panel census (TASK-090 HUD A-Spec) ----
//
// `build_hud_panel` selects one element by its measured placement. The census instead enumerates every draw of the
// panel pass of one stream: the PATH2 sprites between the present blit and the next full-screen batch, and the PATH1
// draws of the tap that write the CT16 display buffer inside the stream's job range. Each entry carries the captured
// register state and geometry, so the caller can classify the HUD elements (position/lap, times, tachometer, speed,
// gear, messages, fuel/tyres) and run each entry's texture through the same HudTextureCache the renderer uses.
struct HudCensusEntry {
  bool path1 = false;
  std::uint64_t job = 0;
  std::uint32_t sequence = 0;  // PATH2: the batch sequence; PATH1: the tap record ordinal
  HudDrawState state;
  std::uint32_t vertices = 0;
  float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
  bool axis_aligned = false;  // a top-left/bottom-right sprite pair or the measured axis-aligned plate quad
  bool drawing = true;        // at least one drawing vertex
};

struct HudCensus {
  std::uint64_t stream_vblank = 0;
  std::vector<HudCensusEntry> entries;
  std::uint64_t path2_batches = 0;      // every PATH2 batch between the present blit and the trailing clear
  std::uint64_t path2_drawing = 0;      // of those, the batches with a drawing vertex
  std::uint64_t path1_display_draws = 0;  // tap draws into the CT16 display buffer
  std::uint64_t path1_axis_aligned = 0;   // of those, the measured axis-aligned quad form
  std::map<std::string, std::uint64_t> reasons;
};

bool build_hud_census(const std::vector<GsDecodeBatch> &batches, const std::vector<std::uint8_t> &tap_data,
                      const std::vector<TapFileRecord> &tap_records, std::uint64_t stream_vblank,
                      HudCensus &census, std::string &error);

// ---- Textures ----

// The stream's host->local uploads as a local-memory image: the S3 texture source (the panel's own frame uploads the
// atlas pages through PATH3 IMAGE packets; the captured bytes are preferred over any disc copy).
struct HudUploadCounters {
  std::uint64_t uploads = 0;  // IMAGE packets in the selected streams
  std::uint64_t applied = 0;
  std::uint64_t bytes = 0;
  std::uint64_t refused = 0;
  std::map<std::string, std::uint64_t> reasons;
};

// Builds a 4 MiB GS local-memory image from the host->local IMAGE uploads of `stream_vblank` (only that stream by
// default; with `include_previous`, every captured stream up to it, so a page uploaded before the window is present).
// `base` (optional) seeds the image and its covered set: the sidecar's frame record supplies the pages the stream does
// not re-upload, and the stream's own bytes then overwrite what the frame does upload (the measured order). The image's
// covered set is the union of the destination pages the uploads wrote; a decode that reads a block outside it is
// refused by the decoder instead of reading the zero fill.
bool build_hud_stream_vram(const std::vector<GsDecodeImageUpload> &uploads, std::uint64_t stream_vblank,
                           bool include_previous, SlmImage &slm, HudUploadCounters &counters, std::string &error,
                           const SlmImage *base = nullptr);

// ---- The font atlas page ----
//
// The race panel's digits sample the GT4meter font atlas: a T4 page (128 texels wide, 64 rows, TBW 2) at TBP0 0x3A20
// whose glyph texels the game uploads before the captured race window. The captured stream carries only the page's
// palette bundle (the four CT32 transfers at CBP), so a stream-only local memory holds the page as zero fill and the
// texture decoder refuses the digits as a constant page.
//
// `HudFontAtlas` carries the 8192-byte T4 page (128x128 texels, 32 blocks) exactly as GS local memory holds it: the
// glyph texels of the game's own font. The measured source is the reference renderer's decoded texture for the same
// frame (Format G key bf0800b76cfeb19a-855d33f8da421ba0), whose palette alpha values are exactly the frame's captured
// CSM1 bank; the page is built from that decoded texture with the GS T4 layout (build/evidence-hud-font/ in the HUD
// font task). The file format is little endian:
//
//   u32 magic 'GT4F' (0x46345447), u32 version 1,
//   u32 tbp0, u32 tbw, u32 tw, u32 th, u64 clamp,
//   u32 page_bytes (8192), u32 reserved (0), then page_bytes bytes.
//
// The page and the palette share the page's blocks (the digit draw reads its 16-entry bank at CBP 0x3A27 = the page's
// unused rows 48..63). `apply_hud_font_atlas` therefore saves and restores the 64 bytes of the 16-entry CSM1 bank the
// draw samples (`palette_cbp`), so applying the atlas never replaces the frame's captured palette with page bytes.
struct HudFontAtlas {
  std::uint32_t tbp0 = 0;
  std::uint32_t tbw = 0;
  std::uint32_t tw = 0;
  std::uint32_t th = 0;
  std::uint64_t clamp = 0;
  std::vector<std::uint8_t> page;  // 8192 bytes: the 128x128 T4 page as GS local memory holds it
};

// Parses the sidecar. Every field outside the measured GT4meter geometry is refused by name: magic, version, reserved,
// TBW 2, TW 7, TH 6, page size 8192 and the 0..127 x 0..38 CLAMP region.
bool parse_hud_font_atlas(const std::vector<std::uint8_t> &bytes, HudFontAtlas &atlas, std::string &error);

// Writes the page into `slm` at `target_tbp0` (the draw's TBP0; the GT4meter page rotates through VRAM, so the A-Spec
// gear/speed digits sample it at another page than the night capture's), covers its 32 blocks as texel content and
// preserves the 16-entry (64-byte) CSM1 palette bank at `palette_cbp` for a T4 draw with CSA 0. A page that does not
// fit the 4 MiB image is refused.
bool apply_hud_font_atlas(const HudFontAtlas &atlas, SlmImage &slm, std::uint32_t target_tbp0,
                          std::uint32_t palette_cbp, std::string &error);

enum class HudTextureStatus : std::uint32_t {
  ok = 0,
  refused_format = 1,   // PSM is not T4/T8
  refused_state = 2,    // palette/CLUT state outside the measured CSM1 CT32 set
  refused_decode = 3,   // the sidecar lacks a block or the decoder refused the record
};

struct HudTexture {
  HudTextureStatus status = HudTextureStatus::refused_format;
  std::string detail;
  std::uint64_t tex0 = 0, tex1 = 0, clamp = 0;  // the request's content key (TEX0 with CLD cleared)
  std::uint32_t width = 0, height = 0, x0 = 0, y0 = 0;  // decoded rectangle in texture coordinates
  std::vector<std::uint8_t> rgba;  // RGBA8 row-major, top row first
};
const char *hud_texture_status_name(HudTextureStatus status) noexcept;

// Decodes one T4/T8 texture with its CT32 CSM1 palette from the captured local memory, cached by the full TEX0 state.
// The rectangle is the CLAMP region (the measured reads always use region clamp), so the decoded rectangle starts at
// (minu, minv).
class HudTextureCache {
 public:
  explicit HudTextureCache(const SlmImage &slm) : slm_(slm) {}
  // Marks a TBP0 of a page a GT4F font atlas supplied. The page is judged on RGBA (the measured GT4meter palette is
  // white, so its glyphs differ only in the alpha lane); every page without an atlas and without a captured T4/T8
  // texel upload keeps the S3 RGB constant-page refusal (a stream-only font page holds the palette bundle's bytes as
  // indices, all of them white).
  void set_font_atlas_page(std::uint32_t tbp0) { font_atlas_pages_.insert(tbp0); }
  const HudTexture &get(const HudDrawState &state, std::string &error);
  std::uint64_t decodes() const noexcept { return decodes_; }
  std::uint64_t cache_hits() const noexcept { return cache_hits_; }
  std::uint64_t refusals() const noexcept { return refusals_; }
  const std::map<std::string, std::uint64_t> &reasons() const noexcept { return reasons_; }

 private:
  const SlmImage &slm_;
  std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, HudTexture> cache_;
  std::uint64_t decodes_ = 0, cache_hits_ = 0, refusals_ = 0;
  std::set<std::uint32_t> font_atlas_pages_;  // every page a GT4F atlas supplied (the page rotates through VRAM)
  std::map<std::string, std::uint64_t> reasons_;
};

// ---- Drawing ----

struct HudDrawOptions {
  // The display buffer is CT16: the destination is read as its 5-bit value shifted left by 3 (the reference frame's
  // own expansion, measured on the r8 scanout) and the result is stored the same way.
  bool ct16 = true;
  // TASK-090 HUD A-Spec (TASK-214): the 2D canvas the captured element coordinates live in, in scanout pixels. The
  // panel of the captured night race is the game's 640x448 scanout; the output may be any size, and every target pixel
  // centre is mapped back to this canvas (`(p + 0.5) * canvas / target - 0.5`) before coverage, scissor and UV
  // interpolation. At 640x448 the mapping is the identity (the S3 measured case); at 1920x1344 it is the 3x validation
  // scale the CoreGS 2.0 reference dumps use. The fields are the parameterisation TASK-214 needs: a future 16:9/21:9
  // output keeps the game's own canvas coordinates (XYOFFSET and the HUD anchor rules of TASK-079/080) and changes
  // only this mapping, never the captured draw geometry.
  std::uint32_t canvas_width = 640;
  std::uint32_t canvas_height = 448;
};

struct HudDrawCounters {
  std::uint64_t draws = 0;
  std::uint64_t pixels = 0;
  std::uint64_t masked_pixels = 0;
  std::uint64_t sampled = 0;
  std::uint64_t blended = 0;
  std::uint64_t alpha_test_failed = 0;
  std::uint64_t refused = 0;
  std::map<std::string, std::uint64_t> reasons;
};

// Composites the panel over `rgba` (RGBA8, `width` x `height`, the native 640x448 scanout). `mask` receives 255 at
// every pixel a panel draw wrote; `contrib_mask` (optional) receives 255 only where the fragment's alpha is above zero,
// i.e. the pixels the element actually contributes to over the world (the fair A-Spec comparison mask). Returns false
// with `error` only for an invalid target or an unhandled primitive; a per-draw refusal is counted.
bool draw_hud_panel(const HudPanel &panel, HudTextureCache &textures, std::vector<std::uint8_t> &rgba,
                    std::uint32_t width, std::uint32_t height, const HudDrawOptions &options,
                    std::vector<std::uint8_t> *mask, HudDrawCounters &counters, std::string &error,
                    std::vector<std::uint8_t> *contrib_mask = nullptr);

// The measured reference expansion of a CT16 scanout value (5 bits stored, the reference's `<< 3`).
std::uint8_t ct16_expand(std::uint8_t value8) noexcept;

// Human-readable panel report (selection, counters, refusals).
std::string hud_panel_report(const HudPanel &panel, const HudDrawCounters &draws, const HudTextureCache &textures);

}  // namespace gt4::coregs3
