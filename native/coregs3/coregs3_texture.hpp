// TASK-090 CoreGS 3.0: the native decoder of the car-pass and world-pass textures.
//
// The `.gt4scene` capture stores the 40-byte PGLUtexture record of every draw item (five little-endian qwords: TEX0,
// TEX1, MIPTBP1, MIPTBP2, CLAMP) and the `.gt4vram` sidecar stores the GS local memory blocks. This unit joins the two:
// it parses the record, locates the texels and the palette in the local memory, decodes RGBA8 and returns a content key.
// TASK-090 track textures adds a second entry point, `decode_texture_state`, that takes the TEX0/TEX1/CLAMP/TEX2 words
// the VU1 tap measured at the draw instead of the record: the world stream writes TEX0 with CLD = 0 and uploads the
// palette bank through TEX2, which the record path does not model.
//
// It is Vulkan-free and reuses the MIT renderer pieces instead of a second decoder:
//   * `hwgs::memory::LocalMemory` (native/hw_gs/memory) for the GS swizzle and pixel addressing;
//   * `hwgs::clut::ClutUnit::PeekLoad` and `hwgs::clut::DecodeHalfwords` (native/hw_gs/clut) for the CSM1 palette. The
//     full 16x16 32-bit palette bank is loaded unshifted and DecodeHalfwords applies TEX0.CSA, because the measured car
//     pass proves CSA selects a bank group at sampling time (draw 2, CSA 0, and draw 3, CSA 6, both reproduce the
//     CoreGS 2.0 texture dump byte for byte);
//   * `hw_gs_replace` (native/hw_gs/replace) for the CLUT buffer, the direct-format expansion and the Format G key.
// The RGBA loop is the one of the CoreGS 2.0 texture dump (core2_renderer.cpp, DumpTexture), over the same source.
//
// Refusals are explicit and never guessed: only the formats and states the car pass measured are decoded (T4, T8 and
// CT32 textures, CT32/CSM1 palettes, no mipmaps), and every other state returns a status that names the reason.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hw_gs_replace/clut.hpp"
#include "hw_gs_replace/gs_types.hpp"
#include "coregs3_vram.hpp"
#include "hw_gs_replace/keys.hpp"
#include "hw_memory.hpp"

namespace gt4::coregs3 {

// One 40-byte PGLUtexture record (`D_0084AF58 + (a-1)*40`, or the external set) with its TEX0/TEX1/CLAMP fields
// decoded. The raw qwords stay available so the report can print exactly what the capture held.
struct TextureRecord {
  std::uint64_t tex0 = 0;
  std::uint64_t tex1 = 0;
  std::uint64_t miptbp1 = 0;
  std::uint64_t miptbp2 = 0;
  std::uint64_t clamp = 0;

  std::uint32_t tbp0 = 0;  // TEX0: texture base pointer, blocks
  std::uint32_t tbw = 0;   // TEX0: buffer width, 64-pixel units
  std::uint32_t psm = 0;   // TEX0: pixel storage format code
  std::uint32_t tw = 0;    // TEX0: log2 width
  std::uint32_t th = 0;    // TEX0: log2 height
  std::uint32_t tcc = 0;   // TEX0: texture colour component (0 RGB, 1 RGBA)
  std::uint32_t tfx = 0;   // TEX0: texture function (0 MODULATE, 1 DECAL, 2 HIGHLIGHT, 3 HIGHLIGHT2)
  std::uint32_t cbp = 0;   // TEX0: CLUT base pointer, blocks
  std::uint32_t cpsm = 0;  // TEX0: CLUT storage format
  std::uint32_t csm = 0;   // TEX0: CLUT storage method (0 CSM1, 1 CSM2)
  std::uint32_t csa = 0;   // TEX0: CLUT entry offset
  std::uint32_t cld = 0;   // TEX0: CLUT load control
  std::uint32_t lcm = 0;   // TEX1: LOD calculation method
  std::uint32_t mxl = 0;   // TEX1: maximum mip level
  std::uint32_t mmag = 0;  // TEX1: magnification filter
  std::uint32_t mmin = 0;  // TEX1: minification filter
  std::uint32_t wms = 0;   // CLAMP: wrap mode S
  std::uint32_t wmt = 0;   // CLAMP: wrap mode T
  std::uint32_t minu = 0;  // CLAMP: region minimum S
  std::uint32_t maxu = 0;  // CLAMP: region maximum S
  std::uint32_t minv = 0;  // CLAMP: region minimum T
  std::uint32_t maxv = 0;  // CLAMP: region maximum T
};

// Parses a raw record. `size` must be at least the 0x28 bytes of the record.
bool parse_texture_record(const std::uint8_t *bytes, std::size_t size, TextureRecord &record, std::string &error);

// One `.gt4vram` record: the GS local memory blocks it carries.
// `kind`/`pass`/`label` come from a version 2 record (TASK-090 pass textures): a pass-begin record carries the capture
// frame's own VBlank and its PassId, a draw record its `CapturedDraw::sequence` and the texture/palette windows; a
// version 1 record is a vblank record (kind 0, pass 0, label 0, one window). `bytes` hold the windows concatenated in
// `windows` order.
struct VramRecord {
  std::uint32_t kind = 0;   // gt4::coregs3::vram::RecordKind
  std::uint32_t pass = 0;   // PassId for a pass-begin record
  std::uint32_t label = 0;  // CapturedDraw::sequence for a draw record
  std::uint64_t vblank = 0;
  std::vector<vram::Window> windows;
  std::vector<std::uint8_t> bytes;  // sum(window.block_count) * 256, windows concatenated in order

  std::uint32_t block_count() const noexcept {
    std::uint32_t total = 0;
    for (const vram::Window &window : windows) total += window.block_count;
    return total;
  }
};

// Reads and validates every GT4V record of a sidecar file (bounds: 4096 records / 512 MiB).
bool read_vram_sidecar(const std::string &path, std::vector<VramRecord> &records, std::string &error);
// The same parse over bytes already in memory (the fixture uses this form).
bool parse_vram_sidecar(const std::uint8_t *data, std::size_t size, std::vector<VramRecord> &records,
                        std::string &error);

// A 4 MiB GS local memory image with the block range the sidecar covered. A decode that would read a block outside
// that range is refused as truncated instead of decoding the zero fill.
struct SlmImage {
  hwgs::memory::LocalMemory local;
  // The blocks this image really holds (a VRAM record's windows). A decode that would read a block outside this set
  // is refused as truncated instead of decoding the zero fill.
  std::vector<vram::Window> covered;
  // TASK-090 HUD A-Spec: the blocks filled by a T4/T8 host->local IMAGE upload of the captured stream, i.e. texel
  // bytes the game itself sent inside the window (as opposed to a palette/CT32 upload or the zero fill of a page
  // uploaded before the capture). A page covered here is real captured texel content, so a constant page is a
  // legitimate solid texture, not the missing-font-atlas placeholder.
  std::vector<vram::Window> texel_covered;
  bool covers(std::uint32_t block) const;
  void cover(const vram::Window &window);
  bool covers_texels(std::uint32_t block) const;
  void cover_texels(const vram::Window &window);
};

// Builds the image of one sidecar record: zero-filled 4 MiB with the record's windows copied into place.
bool build_slm_image(const VramRecord &record, SlmImage &image, std::string &error);
// TASK-090 pass textures: copies one draw record's windows over an existing image (the frame-end default), so the
// patches carry the content that draw sampled. The copied windows are added to the image's covered set.
bool apply_vram_record(const VramRecord &record, SlmImage &image, std::string &error);

// Decode status. `ok` is the only success; every other value names exactly why the texture was not decoded.
enum class TextureStatus : std::uint32_t {
  ok = 0,
  refused_record = 1,     // not a 40-byte PGLUtexture record
  refused_psm = 2,        // a format this decoder does not handle (only T4, T8 and CT32 are measured)
  refused_size = 3,       // TW or TH above 10 (the task's decode bound)
  refused_state = 4,      // a state not carried or not measured (mipmaps, CSM2, CLD, CPSM, CSA above 15)
  refused_truncated = 5,  // the sidecar does not cover a texture or palette block
  refused_palette = 6,    // the palette could not be built
};
const char *texture_status_name(TextureStatus status) noexcept;

struct DecodedTexture {
  TextureStatus status = TextureStatus::refused_record;
  std::string detail;  // the refusal reason in words, or the decoded state for a success

  TextureRecord record;
  // TASK-090 texture mip: levels 1..TEX1.MXL decoded from the MIPTBP1/MIPTBP2 addresses, in level order. Level 0 is the
  // base rectangle above. Empty when the record does not ask for mips or when a level could not be decoded; the latter
  // sets mip_levels_missing so the caller can fall back to level 0 explicitly instead of sampling a half chain.
  struct MipLevel {
    std::uint32_t level = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // RGBA8, row-major, top row first
  };
  std::vector<MipLevel> mip_levels;
  std::uint32_t mip_levels_missing = 0;  // levels 1..MXL the image did not cover completely
  std::uint64_t miptbp1 = 0;             // the words the chain was decoded from (tap, else the record)
  std::uint64_t miptbp2 = 0;
  bool miptbp_from_state = false;
  // TASK-090 track M1: TEX1.MXL was above 0 and the caller allowed the level-0 fallback (the world pass). The bytes are
  // level 0 only; the flag is evidence for the report, never a silent substitution.
  bool level0_mip_fallback = false;
  // TASK-090 track textures: the palette bank was loaded by the TEX2 write that precedes the draw (TEX0.CLD = 0), not by
  // the TEX0 write itself. The decoded bytes are the same bank; the flag is evidence for the report.
  bool palette_via_tex2 = false;
  std::uint32_t width = 0;   // decoded rectangle (the CLAMP region when it is present, else the full size)
  std::uint32_t height = 0;
  std::uint32_t x0 = 0;      // top-left of the decoded rectangle in texture coordinates
  std::uint32_t y0 = 0;
  std::uint32_t palette_entries = 0;  // 16, 256, or 0 for a direct format
  std::vector<std::uint8_t> rgba;     // RGBA8 bytes, row-major, top row first (R, G, B, A)

  // Format G content key (native/hw_gs/replace/include/hw_gs_replace/keys.hpp): the payload hash of the base rectangle,
  // the palette hash for indexed formats (0 for direct) and the hashed width/height. The address (TBP0/CBP) is not
  // part of the key, so the same bytes at another base keep it; a payload or palette byte change breaks it.
  hw_gs_replace::FormatGKey key;
  // The Format P key and name the CoreGS 2.0 `GT4_HW_DUMP_TEXTURES` reference uses (keys.hpp, names.hpp), computed
  // from the same rectangle and palette. With no upload extent in the record this hashes the base level alone, which
  // is exactly what the reference dump hashes for a texture that was not uploaded in the draw's last ten batches.
  hw_gs_replace::FormatPKeys format_p;
  std::string format_p_name;
};

// TASK-090 track textures: the GS sampling state of one draw item, as the VU1 tap emitted it. The values are the
// canonical 64-bit register writes in force at the item's first vertex: TEX0_1/2, TEX1_1/2 and CLAMP_1/2, plus the last
// TEX2_1/2 write (the CLUT load descriptor; the world stream writes TEX0 with CLD = 0 and uploads the palette bank
// through TEX2 with CLD = 1). A register the tap never wrote keeps its `*_known` flag false; the caller then falls back
// to the item's 40-byte PGLUtexture record.
struct TextureState {
  std::uint64_t tex0 = 0;
  std::uint64_t tex1 = 0;
  std::uint64_t clamp = 0;
  std::uint64_t tex2 = 0;
  // TASK-090 texture mip: the two mip base-pointer registers in force at the item's first vertex. The world stream does
  // not have to rewrite them per draw (they belong to the texture set), so they are optional: the caller falls back to
  // the item's 40-byte PGLUtexture record when the tap never saw a write, and counts which source it used.
  std::uint64_t miptbp1 = 0;
  std::uint64_t miptbp2 = 0;
  bool tex0_known = false;
  bool tex1_known = false;
  bool clamp_known = false;
  bool tex2_known = false;
  bool miptbp1_known = false;
  bool miptbp2_known = false;

  // True when the three sampling registers the decoder needs are present.
  bool sampling_registers_known() const noexcept { return tex0_known && tex1_known && clamp_known; }
  // True when the tap wrote any sampling register: a present but incomplete state is a refusal, not a record fallback.
  bool any_register_known() const noexcept { return tex0_known || tex1_known || clamp_known || tex2_known; }
};

// Decodes one texture record against a local memory image. `record_bytes` is the 40-byte PGLUtexture record; `size` is
// its available byte count (a shorter record is refused). Returns `out.status` (also returned).
TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             DecodedTexture &out, std::string &error);

// The decode options. Only the world pass (TASK-090 track M1, then the texture-mip round) enables the mip handling: a
// record with TEX1.MXL above zero is either decoded with its full MIPTBP chain (`decode_mip_levels`) or at level 0 with
// the fact recorded in DecodedTexture::level0_mip_fallback. The default refuses it, exactly as the car pass did.
struct TextureDecodeOptions {
  bool level0_mip_fallback = false;
  // TASK-090 texture mip: decode levels 1..MXL from MIPTBP1/MIPTBP2. A level the image does not cover drops the whole
  // chain and sets level0_mip_fallback, so a half chain is never sampled.
  bool decode_mip_levels = false;
  // The MIPTBP1/MIPTBP2 words to use when the tapped state carries none (the item's 40-byte PGLUtexture record). The
  // caller sets `mip_registers_known` only when it can name the source; DecodedTexture::miptbp_from_state reports which
  // source was used.
  bool mip_registers_known = false;
  std::uint64_t mip_bp1 = 0;
  std::uint64_t mip_bp2 = 0;
};
TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             const TextureDecodeOptions &options, DecodedTexture &out, std::string &error);

// TASK-090 track textures: decodes the item's texture from the GS state the tap measured, instead of from the 40-byte
// record. The TEX0/TEX1/CLAMP fields replace the record's; the format, size, palette, rectangle and key rules are the
// record decoder's, so the two paths cannot drift. For an indexed texture:
//   * TEX0.CLD = 1 keeps the record path's measured rule (the TEX0 write loads the palette from CBP);
//   * TEX0.CLD = 0 requires a TEX2 write with CLD = 1 (the measured world stream: the VU1 uploads the palette bank
//     through TEX2 at CSA 0, with the 16-entry T4 load class when a T4 texture samples CSA 0 and the 256-entry T8 class
//     otherwise), and TEX2's CBP/CPSM/CSM must equal TEX0's, so the bank the decoder reads is the bank the GS loaded.
//     Anything else is an explicit `refused_state` naming the register that is outside the measured set, never a guess.
// `state` must carry TEX0, TEX1 and CLAMP; an incomplete state is refused with the missing register named.
TextureStatus decode_texture_state(const SlmImage &slm, const TextureState &state, const TextureDecodeOptions &options,
                                   DecodedTexture &out, std::string &error);

}  // namespace gt4::coregs3
