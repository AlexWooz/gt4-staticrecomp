// TASK-090 CoreGS 3.0: the native decoder of the car-pass textures.
//
// The `.gt4scene` capture stores the 40-byte PGLUtexture record of every draw item (five little-endian qwords: TEX0,
// TEX1, MIPTBP1, MIPTBP2, CLAMP) and the `.gt4vram` sidecar stores the GS local memory blocks. This unit joins the two:
// it parses the record, locates the texels and the palette in the local memory, decodes RGBA8 and returns a content key.
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

// One `.gt4vram` record: the block range of the GS local memory image it carries.
struct VramRecord {
  std::uint64_t vblank = 0;
  std::uint32_t first_block = 0;
  std::uint32_t block_count = 0;
  std::vector<std::uint8_t> bytes;  // block_count * 256, block `first_block` first
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
  std::uint32_t first_block = 0;
  std::uint32_t block_count = 0;
  bool covers(std::uint32_t block) const;
};

// Builds the image of one sidecar record: zero-filled 4 MiB with the record's blocks copied into place.
bool build_slm_image(const VramRecord &record, SlmImage &image, std::string &error);

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
  // TASK-090 track M1: TEX1.MXL was above 0 and the caller allowed the level-0 fallback (the world pass). The bytes are
  // level 0 only; the flag is evidence for the report, never a silent substitution.
  bool level0_mip_fallback = false;
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

// Decodes one texture record against a local memory image. `record_bytes` is the 40-byte PGLUtexture record; `size` is
// its available byte count (a shorter record is refused). Returns `out.status` (also returned).
TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             DecodedTexture &out, std::string &error);

// The decode options. Only the world pass (TASK-090 track M1) sets `level0_mip_fallback`: a record with TEX1.MXL above
// zero is decoded at level 0 and the fact is recorded in DecodedTexture::level0_mip_fallback. The default refuses it,
// exactly as the car pass did.
struct TextureDecodeOptions {
  bool level0_mip_fallback = false;
};
TextureStatus decode_texture(const SlmImage &slm, const std::uint8_t *record_bytes, std::size_t size,
                             const TextureDecodeOptions &options, DecodedTexture &out, std::string &error);

}  // namespace gt4::coregs3
