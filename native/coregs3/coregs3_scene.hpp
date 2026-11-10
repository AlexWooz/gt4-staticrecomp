// TASK-090 CoreGS 3.0, phase 1: the scene snapshot and the `.gt4scene` capture format.
//
// The EE-side hooks (native/coregs3/coregs3_hooks.cpp, docs/COREGS3-PLAN.md §5.2 K1..K5) copy one race frame's car
// draws out of guest memory into this bounded, immutable shape. It is pure data: the hooks never write guest memory and
// never call guest code, so the simulation digest, the race clock and the read-backs stay identical with the switch off,
// in shadow mode and in overlay mode.
//
// Everything here is raw guest values (words, float bit patterns, guest addresses). Interpretation belongs to the
// offline reader and to the renderer, so a capture stays readable when an interpretation changes. Captures and captures
// derived from the user's disc are never committed (they live under `private/` or `build/evidence/`).
//
// File layout (little endian, all sizes in bytes):
//
//   header     magic 'G','T','4','S' (u32 0x53345447), u32 version, u32 target, u32 reserved
//   frame      u32 block_bytes (of the whole frame block, this word included), u32 frame_magic 'G','F','3','G',
//              u32 index, u32 reserved, u64 vblank,
//              u32 draw_count, u32 texture_count, u32 material_count, u32 packet_bytes,
//              camera: u32 valid, u32 camera, u32 pgl_block_k3[16], u32 pgl_block_k4[16], u32 vf_k4[32]
//              textures [texture_count]:  u32 guest_address, u8 bytes[0x28]
//              materials[material_count]: u32 guest_address, u8 bytes[0x50]
//              draws    [draw_count]:    CapturedDraw (fixed size, see serialize_draw)
//              packets  [packet_bytes]:  the VIF bytes of every item, in draw and item order, 16-byte aligned
//
// A reader that only needs the counters may skip a block by block_bytes. kCaptureTargetV200 is the only target the
// hooks currently produce; a capture from another target is refused rather than guessed.
//
// Format version 2 (2026-11-09) adds the per-draw VU1 light block (CapturedDraw::light_block): the qwords the shape call
// uploads to TOPS+0xC9, which is what the VU1 lighting fragment consumes. The draw record grew by a fixed
// kMaxLightBlockQwords*16 + 12 bytes, so a version 1 reader cannot walk a version 2 block. There is no migration: the
// version check refuses a file it was not built for (recapture with a matching build), which is the format's rule for an
// interpretation the reader does not know.
//
// Format version 3 (2026-11-09) adds the per-draw VU1 program selector (CapturedDraw::program_*): the ITOP and MSCAL
// words the PGL writes in the terminating tag of the draw's REF chain, which name the microprogram entry the VU1 runs.
// Seven words per draw; versions 1 and 2 are refused the same way.
//
// Format version 4 (2026-11-10) adds the per-item VU1 light state and program run pair (CapturedItem::light_*,
// run_*) and the per-draw chain summary (chain_tags/chain_valid/program_uniform), and changes the pack of the draw
// record: every item record grows by a fixed 16-qword block plus seven words. Version 4 is the first version whose
// light state is complete for the items the VU1 runs without an upload: such an item records the tracked carried block
// (the TOPS+0xC9 content of its MSCAL parity, tracked in VIF command order), because the VU1 keeps the previous block
// when a shape uploads nothing. It is also the first version whose program selector is per item: the measured capture
// shows the ITOP word changes within some draws (a draw can mix programs), so `program_*` alone was a last-item view.
// Versions 1..3 are refused, not migrated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

// `GT4_COREGS3=off|shadow|overlay|present`. `off` and `shadow` are the observe-only modes; `overlay` refuses to start
// until the in-process seam lands (the plan's level 1); `present` is the debug-only live layer of docs/COREGS3-HOOKS.md
// §10.3 that draws the captured car pass over the composed image after the compose, never an acceptance mode.
enum class Mode : std::uint32_t { off = 0, shadow = 1, overlay = 2, present = 3 };

Mode parse_mode(const char *text) noexcept;
const char *mode_name(Mode mode) noexcept;

inline constexpr std::uint32_t kSceneMagic = 0x53345447u;    // "GT4S"
inline constexpr std::uint32_t kFrameMagic = 0x47334647u;    // "GF3G"
inline constexpr std::uint32_t kSceneVersion = 4u;
inline constexpr std::uint32_t kCaptureTargetV200 = 0u;

// The VU1 light block of one shape call lands at TOPS qword 0xC9 through a VIF UNPACK whose payload is at most nine
// qwords (six for the material/light form of func_0049EB90/func_0049F170, nine for func_0049E698). The capture reserves
// sixteen qwords so an unfamiliar but valid UNPACK into 0xC9 is stored rather than truncated; a tag that claims more is
// refused and counted.
inline constexpr std::uint32_t kMaxLightBlockQwords = 16u;

// Bounds. They are capacities, not formats: a frame that would exceed one is refused as a whole (the plan's
// per-frame fallback) and counted, never truncated.
inline constexpr std::uint32_t kMaxDrawsPerFrame = 4096u;
inline constexpr std::uint32_t kMaxItemsPerDraw = 256u;
inline constexpr std::uint32_t kMaxTextureRecords = 512u;
inline constexpr std::uint32_t kMaxMaterialRecords = 256u;
inline constexpr std::uint32_t kMaxPacketBytesPerFrame = 64u * 1024u * 1024u;
inline constexpr std::uint32_t kMaxPacketBytesPerDraw = 1024u * 1024u;
inline constexpr std::uint32_t kTextureRecordBytes = 0x28u;
inline constexpr std::uint32_t kMaterialRecordBytes = 0x50u;

// The PGL context object lives at guest scratchpad 0x70002000 (`func_0049D308` passes exactly that address as `obj`).
// The hook copies these raw spans; `kPglWords` is their total and is part of the capture format.
struct PglSpan {
  std::uint32_t offset;
  std::uint32_t words;
};
inline constexpr PglSpan kPglSpans[] = {
    {0x000u, 20u},  // flag, count, x, y, value (float bits), mask, flags
    {0x1A0u, 32u},  // GS shadow: scissor, alpha, depth, blend, texture and clamp words
    {0xA60u, 20u},  // material block written by func_0049E228 (4 qwords) plus its scale float
    {0xFA0u, 16u},  // the second 4-qword upload block of func_0049E548 (role still Unknown)
    {0x1020u, 3u},  // nc, nd, ne counters
};
inline constexpr std::uint32_t kPglSpanCount = sizeof(kPglSpans) / sizeof(kPglSpans[0]);
inline constexpr std::uint32_t kPglWords = 91u;

inline constexpr std::uint32_t kNoRecord = 0xFFFFFFFFu;

// Guest scratchpad word offsets used by the hooks. Names follow the decompiled structures in
// `src/network/unit_00494578/` (Obj, Item, Cmd) and are documented in docs/COREGS3-HOOKS.md.
inline constexpr std::uint32_t kPglContext = 0x70002000u;
inline constexpr std::uint32_t kObjFlag = 0x03u;
inline constexpr std::uint32_t kObjCount = 0x14u;
inline constexpr std::uint32_t kObjX = 0x18u;
inline constexpr std::uint32_t kObjY = 0x1Cu;
inline constexpr std::uint32_t kObjValue = 0x20u;
inline constexpr std::uint32_t kObjMask = 0x40u;
inline constexpr std::uint32_t kObjBits = 0x1D8u;
inline constexpr std::uint32_t kObjBits2 = 0x1E8u;
inline constexpr std::uint32_t kObjOut = 0x1F8u;
inline constexpr std::uint32_t kObjCallback = 0x21Cu;
inline constexpr std::uint32_t kObjMatrix = 0xFA0u;
inline constexpr std::uint32_t kObjNc = 0x1020u;

// The Cmd header (func_0049D328: `int a; float value; u8 flag, bits; u16 count, nc, nd; Item item[]`).
inline constexpr std::uint32_t kCmdHeaderBytes = 0x10u;
inline constexpr std::uint32_t kCmdItemBytes = 0x08u;
inline constexpr std::uint32_t kCmdValue = 0x04u;
inline constexpr std::uint32_t kCmdFlag = 0x08u;
inline constexpr std::uint32_t kCmdBits = 0x09u;
inline constexpr std::uint32_t kCmdCount = 0x0Au;
inline constexpr std::uint32_t kCmdNc = 0x0Cu;
inline constexpr std::uint32_t kCmdNd = 0x0Eu;
inline constexpr std::uint32_t kCmdItems = 0x10u;

// The PGL record tables the per-item callback resolves (`func_0049CEA0`): 9 bits of texture index and 7 bits of material
// index in the item's `event` halfword. 0x1FF/0x7F mean "external table", where the index is added to a live cursor.
inline constexpr std::uint32_t kTextureIndexMask = 0x1FFu;
inline constexpr std::uint32_t kMaterialIndexShift = 9u;
inline constexpr std::uint32_t kMaterialIndexMask = 0x7Fu;
inline constexpr std::uint32_t kExternalTextureIndex = 0x1FFu;
inline constexpr std::uint32_t kExternalMaterialIndex = 0x7Fu;
// Guest globals read by func_0049CEA0: the record arrays and their external cursors.
inline constexpr std::uint32_t kTextureArray = 0x0084AF58u;
inline constexpr std::uint32_t kMaterialArray = 0x0084AF5Cu;
inline constexpr std::uint32_t kExternalTextureArray = 0x0084AF60u;
inline constexpr std::uint32_t kExternalMaterialArray = 0x0084AF64u;
inline constexpr std::uint32_t kTextureCursor = 0x00624C7Cu;   // D_00624C7C, added to an external texture index
inline constexpr std::uint32_t kMaterialCursor = 0x00624C80u;  // D_00624C80, added to an external material index

// Where one item's VU1 light block came from (CapturedItem::light_source; format version 4).
enum LightItemSource : std::uint32_t {
  kLightItemNone = 0,              // no upload observed in the captured stream: the TOPS+0xC9 state is unknown
  kLightItemUploaded = 1,          // this item's own light tag uploaded the block (the literal VU1 input)
  kLightItemCarried = 2,           // no tag for this item: the tracked last upload before its MSCAL (VU1 carried state)
  kLightItemCarriedUncertain = 3,  // carried, but a PGL output-cursor discontinuity followed the tracked upload
};

// One item of the Cmd: a DMA REF to the model's own VIF packet (`func_0049D328`: p->a = count|0x30000000,
// p->b = addr), plus the texture/material indices of its `event`.
struct CapturedItem {
  std::uint32_t address = 0;       // guest address of the VIF packet (masked with 0x0FFFFFFF by the game)
  std::uint16_t qwords = 0;        // DMA REF QWC, 16-byte units
  std::uint16_t event = 0;         // tex_index | material_index << 9
  std::uint32_t texture_record = kNoRecord;   // index into the frame's texture table
  std::uint32_t material_record = kNoRecord;  // index into the frame's material table
  std::uint32_t packet_offset = 0;            // byte offset inside the frame's packet block
  std::uint32_t packet_bytes = 0;             // bytes copied (16-byte aligned; 0 when the item was not copied)
  // The light block this item's VU1 microprogram run reads at TOPS+0xC9 (format version 4). `func_0049D328` writes a
  // REF tag whose tag2 word is the UNPACK that uploads the block; `func_0049D1A0` copies it only while its per-draw
  // counter is nonzero (mode starts at 2, so the first two items of a draw; every item of the Cmd kinds that re-prepare
  // the block per item, `flag & 0x1F` 1 and 4 with Obj+0x48 flags bit 0). The other items upload nothing: the VU1 keeps
  // the block of the last upload with the same TOPS/MSCAL parity, which the capture tracks across the command stream and
  // records here with `light_source` = carried. A draw whose Cmd kind byte (`flag & 0x1F`) is 0 uploads nothing at all
  // (func_0049D080 zeroes the tag at Obj+0x30 and no case helper sets it again). `light_block_qwords` is 0 when nothing
  // is known (the VU1 data memory before the first observed upload is not read: see docs/COREGS3-HOOKS.md §13).
  std::uint32_t light_block[kMaxLightBlockQwords][4] = {};
  std::uint32_t light_block_qwords = 0;
  std::uint32_t light_block_command = 0;  // the tag's raw UNPACK word, e.g. 0x6C06C0C9; 0 when carried/unknown
  std::uint32_t light_block_source = 0;   // guest address the tag uploaded from; the tracked upload's source carried
  std::uint32_t light_source = kLightItemNone;  // LightItemSource
  // The ITOP/MSCAL pair that runs this item: the tag2 words (word 2 = VIF ITOP, word 3 = VIF MSCAL) of the tag that
  // follows this item's REF tag (the next item's tag, or the terminating tag), raw. `run_valid` is 1 only when both
  // words match the VIF forms. `program_*` of the draw carries the terminating pair for compatibility.
  std::uint32_t run_itop = 0;
  std::uint32_t run_mscal = 0;
  std::uint32_t run_valid = 0;
};

// One `pgluCallShape` call of the car pass.
struct CapturedDraw {
  std::uint32_t sequence = 0;  // per-frame draw ordinal, in guest call order
  std::uint32_t pass = 0;      // PassId
  std::uint32_t shape = 0;     // a0: the Cmd
  std::uint32_t cmd_a = 0;     // Cmd + 0x00
  std::uint32_t cmd_value = 0; // Cmd + 0x04, float bits
  std::uint32_t cmd_flag = 0;  // Cmd + 0x08 (byte)
  std::uint32_t cmd_bits = 0;  // Cmd + 0x09 (byte)
  std::uint32_t cmd_count = 0; // Cmd + 0x0C (u16)
  std::uint32_t cmd_nc = 0;    // Cmd + 0x0E (u16)
  std::uint32_t cmd_nd = 0;    // Cmd + 0x10 (u16)
  std::uint32_t item_count = 0;
  std::uint32_t pgl[kPglWords] = {};
  std::uint32_t vf[8][4] = {};  // VU0 vf24..vf31 as raw bits, read at the pre-call point
  // The eight qwords func_0049E548 writes to VU1 qword 0xC1 for this draw (four matrix rows plus the second block),
  // read back from the PGL's packet buffer after the call. This is exactly what the VU1 program receives, so it does
  // not depend on knowing which VU0 register held what at the pre-call instant.
  std::uint32_t matrix_packet[8][4] = {};
  std::uint32_t matrix_packet_valid = 0;
  // The light block the VU1 receives at TOPS+0xC9 for this draw, read back from the same PGL output packet buffer after
  // the call: func_0049D328 writes a DMA REF tag there whose VIF UNPACK names the variant, and the tag's address word
  // points at the qwords on the shape's stack. `light_block_qwords` is 0 when the draw carried no such upload (or when
  // it could not be read); the qword count (6 or 9) *is* the variant. The body/paint family uses the six-qword form:
  // 0xC9..0xCB = the 3x3 light matrix M, 0xCC = the light position LP, 0xCD = C3, 0xCE = C4.
  std::uint32_t light_block[kMaxLightBlockQwords][4] = {};
  std::uint32_t light_block_qwords = 0;
  std::uint32_t light_block_command = 0;  // the tag's VIF UNPACK word, e.g. 0x6C06C0C9 or 0x6C09C0C9
  std::uint32_t light_block_source = 0;   // guest address the tag's REF read the qwords from
  // Format version 4: the PGL output chain this draw wrote, walked at the K1 post hook. `chain_tags` is the number of
  // tag qwords seen and `chain_valid` is 1 when the walk parsed exactly from the pre-call cursor to the post-call
  // cursor. `program_uniform` is 1 when every item's run pair is valid and its **ITOP** word equals the terminating
  // tag's ITOP (the real program selector, round 13): measured per-item evidence, which the capture shows is NOT always
  // 1 (the operands are recomputed every iteration, so a draw can mix programs). The MSCAL stub immediate can also
  // differ per item and is captured in CapturedItem::run_mscal.
  std::uint32_t chain_tags = 0;
  std::uint32_t chain_valid = 0;
  std::uint32_t program_uniform = 0;
  // The VU1 program selector, read from the terminating tag of the draw's REF chain in the same PGL output buffer.
  // func_0049D328 writes word 3 of every tag as `0x14000000 | n` (VIF MSCAL) and word 2 as `0x04000000 | (x + y)` (VIF
  // ITOP); the terminating tag carries the last item's words. `n` comes from obj->count (PGL +0x14) and the Cmd
  // flag/bits: the MSCAL entry is a small stub that sets vi15 (the shared-code return address) and vi14 (the 0xD9 block
  // pointer, or 0), then dispatches to ITOP * 8. The ITOP operands (+0x18/+0x1C) are written inside the call by
  // func_0049D080 and its case helpers (func_0049F1F8/func_0049F0F0/...), so the ITOP entry is the real per-shape
  // program and encodes the material/program variant; the capture's pre-call `pgl[6]/pgl[7]` are the previous call's
  // values and must not be read as this draw's ITOP. Both VIF immediates are in 8-byte units, so the byte addresses in
  // the 16 KiB VU1 image are `n * 8` and `ITOP * 8` (the AOT emitter's entry values). `program_count` is the MSCAL
  // source.
  std::uint32_t program_tag = 0;       // raw tag word 3, e.g. 0x140003A8
  std::uint32_t program_selector = 0;  // the VIF MSCAL immediate (program_tag & 0xFFFF)
  std::uint32_t program_entry = 0;     // program_selector * 8: the byte address of the MSCAL entry stub
  std::uint32_t program_itop = 0;      // raw tag word 2, e.g. 0x0400007B: the VIF ITOP (obj->x + obj->y)
  std::uint32_t program_dispatch = 0;  // (program_itop & 0x3FF) * 8: the entry the stub dispatches to
  std::uint32_t program_count = 0;     // PGL obj->count (+0x14) the MSCAL selector is derived from
  std::uint32_t program_valid = 0;     // 1 when the terminating tag carried both VIF words
  CapturedItem items[kMaxItemsPerDraw] = {};
};

struct CapturedTexture {
  std::uint32_t address = 0;
  std::uint8_t bytes[kTextureRecordBytes] = {};
};

struct CapturedMaterial {
  std::uint32_t address = 0;
  std::uint8_t bytes[kMaterialRecordBytes] = {};
};

// K3/K4 (docs/COREGS3-PLAN.md §5.2): the camera call of func_0033EAB8. Phase 1 records the camera object address the
// game passes and the raw PGL blocks at the two return labels, so a later phase can derive the projection without
// guessing here. The block at PGL context +0xFA0 is the second 4-qword upload of func_0049E548; its role is still
// Unknown (plan §3.1), which is exactly why it is captured raw instead of interpreted.
struct CameraSnapshot {
  std::uint32_t valid = 0;      // bit0 K3 (projection) seen, bit1 K4 (modelview) seen, bit2 camera address read
  std::uint32_t camera = 0;     // guest address passed as a0 to the camera virtual calls
  std::uint32_t pgl_block_k3[16] = {};
  std::uint32_t pgl_block_k4[16] = {};
  std::uint32_t vf_k4[8][4] = {};
};

struct FrameSnapshot {
  std::uint32_t index = 0;   // capture-local frame ordinal (0, 1, 2, ...)
  std::uint64_t vblank = 0;  // guest VBlank counter when the car pass began
  std::uint32_t draw_count = 0;
  std::uint32_t texture_count = 0;
  std::uint32_t material_count = 0;
  std::uint64_t overflow_flags = 0;  // one bit per refusal cause, see CaptureCounters
  CameraSnapshot camera;
  std::vector<CapturedDraw> draws;
  std::vector<CapturedTexture> textures;
  std::vector<CapturedMaterial> materials;
  std::vector<std::uint8_t> packets;  // concatenated item packets
};

// Refusals are counted per cause so a frame that falls back can be explained. The bits travel in the snapshot header.
enum Overflow : std::uint64_t {
  kOverflowDraws = 1ull << 0,
  kOverflowItems = 1ull << 1,
  kOverflowTextures = 1ull << 2,
  kOverflowMaterials = 1ull << 3,
  kOverflowPackets = 1ull << 4,
  kOverflowGuestRead = 1ull << 5,
};

// Serializes a whole frame block (see the file layout above). Returns false when the snapshot is inconsistent (a
// packet range outside the packet block, a record index outside its table, a count above its cap), so a damaged
// capture is refused instead of written.
bool frame_block_bytes(const FrameSnapshot &frame, std::vector<std::uint8_t> &output);

// Reads one frame block from `data` (which starts at the frame's block_bytes word). Returns the bytes consumed, or 0 on
// a malformed block. `version` is the file header's version; a version this reader does not know is refused.
std::size_t parse_frame_block(const std::uint8_t *data, std::size_t available, std::uint32_t version,
                              FrameSnapshot &frame, std::string &error);

struct SceneHeader {
  std::uint32_t version = kSceneVersion;
  std::uint32_t target = kCaptureTargetV200;
};

bool write_scene_header(std::vector<std::uint8_t> &output, const SceneHeader &header);
bool parse_scene_header(const std::uint8_t *data, std::size_t available, SceneHeader &header, std::string &error);

// The hook-side counters (F8 category `coregs3_capture`); plain data so a fixture can check them.
struct CaptureCounters {
  std::uint64_t calls = 0;            // pgluCallShape calls seen
  std::uint64_t captured = 0;         // draws stored (car pass only)
  std::uint64_t outside_pass = 0;     // calls outside the car pass (counted, not stored)
  std::uint64_t frames = 0;           // completed frames seen
  std::uint64_t frames_written = 0;   // frames written to the capture file
  std::uint64_t refused_frames = 0;   // frames dropped because a bound was hit
  std::uint64_t overflow = 0;         // bit mask of Overflow causes seen, ever
  std::uint64_t guest_read_failures = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t item_packets = 0;     // items whose VIF packet was copied
  std::uint64_t records = 0;          // texture and material records copied (deduplicated)
  std::uint64_t light_blocks = 0;     // draws whose per-draw TOPS+0xC9 light block was read back (first item's upload)
  std::uint64_t light_blocks_missing = 0;  // draws with a nonzero Cmd flag whose first-item light tag was absent
  std::uint64_t light_blocks_carried = 0;  // draws where no item uploaded a block (the carried state fills every item)
  std::uint64_t light_blocks_none = 0;     // draws where no item has any known light state (nothing observed yet)
  std::uint64_t light_items_uploaded = 0;  // items whose own light tag uploaded the block
  std::uint64_t light_items_carried = 0;   // items filled from the tracked carried state
  std::uint64_t light_items_uncertain = 0; // carried items whose tracked block followed a chain discontinuity
  std::uint64_t light_items_none = 0;      // items with no known light state
  std::uint64_t program_selectors = 0;     // draws whose terminating-tag ITOP/MSCAL words were read back
  std::uint64_t program_selectors_missing = 0;  // draws whose terminating tag was absent or not ITOP+MSCAL
  std::uint64_t program_items = 0;         // items whose own ITOP/MSCAL run pair was captured
  std::uint64_t program_items_missing = 0; // items whose run pair was absent or not ITOP+MSCAL
  std::uint64_t program_item_variants = 0; // items whose ITOP selector differs from the draw's terminating ITOP
  std::uint64_t program_item_mscal_variants = 0;  // items whose MSCAL stub immediate differs from the terminating one
  std::uint64_t chain_calls = 0;           // PGL output chains walked (every hooked shape call, in or out of the pass)
  std::uint64_t chain_gaps = 0;            // chains whose pre-call cursor did not continue the previous post-call one
  std::uint64_t chain_incomplete = 0;      // chains that did not parse exactly to the post-call cursor
  std::uint32_t largest_items = 0;    // largest item table seen in one draw (capacity evidence)
  std::uint32_t largest_draw_bytes = 0;   // largest packet total of one draw
  std::uint32_t largest_frame_bytes = 0;  // largest packet total of one frame
};

}  // namespace gt4::coregs3
