// TASK-090 CoreGS 3.0: the PGL output packet chain, walked at the K1 post hook (format version 4).
//
// func_0049D328 (pgluCallShape) writes one draw's packet into the PGL output buffer (`obj->out`, PGL +0x1F8, in the
// scratchpad at 0x70002000) as a DMA1 chain of 16-byte tag qwords. The chain has this structure (Confirmed against the
// decompiled func_0049D328/func_0049D1A0/func_0049E548/func_0049E5F8/func_0049E698/func_0049EB90/func_0049F0F0/
// func_0049CFE8 and the donor VIF tag2 path):
//
//   * the 0xC1 matrix packet, when func_0049E548 ran: a CNT tag (id 1, QWC 8) whose tag2 word 3 is the VIF UNPACK
//     `0x6C08C0C1`, followed by the eight inline data qwords;
//   * per item: a REF tag (id 3, QWC = the item packet QWC, address = the model packet). Tag words 2/3 are the VIFcode
//     pair the VIF executes before the entry's data (the donor's tag-transfer path sends the last 8 tag bytes first):
//     STMASK 0x20000000 + its mask for item 0, the previous item's ITOP/MSCAL pair otherwise;
//   * per item, conditionally, when the Cmd kind byte (`flag & 0x1F`) selects a light family: a REF tag whose address
//     word points at the block func_0049EB90/func_0049E698/func_0049F0F0/func_0049CFE8 prepared and whose tag2 word 3 is
//     the UNPACK into TOPS+0xC9 (e.g. 0x6C06C0C9). func_0049D1A0 copies this tag while its per-draw counter is nonzero
//     (mode starts at 2, so the first two items; every item of the flag 1/4 re-preparing kinds); the items after it
//     upload nothing and read the TOPS buffer written since the previous MSCAL;
//   * per item, when `obj->bits & obj->bits2`: a CNT tag with the TOPS+0xD9 block inline (func_0049E5F8);
//   * the terminating CNT tag (0x10000000, QWC 0) whose tag words 2/3 are the LAST item's ITOP/MSCAL pair.
//
// VIF command order is the order of this chain: the light tag written between item i's REF tag and the tag that starts
// item i's microprogram (item i+1's tag, or the terminating tag) is the block that program run reads. A draw whose kind
// byte is 0 writes no light tag (func_0049D080 clears the prepared tag at Obj+0x30 and no case helper sets it again), so
// the VU1 keeps the block written since the previous MSCAL. Because TOP/OFFSET double-buffer the VU1 data memory, that
// is the last upload of the same MSCAL parity (CarriedLight below); the walker tracks both slots across calls because
// this order is global to the PGL output buffer, not per draw.
//
// The walker is pure data: the caller supplies the guest read function, so the CTest fixture drives it on
// hand-authored chains with no game data. It never guesses: a chain that does not parse exactly to the post-call
// cursor is reported incomplete and the caller treats the per-draw fields as invalid.
#pragma once

#include <cstddef>
#include <cstdint>

#include "coregs3_scene.hpp"

namespace gt4::coregs3 {

// Reads `bytes` bytes of guest memory at `address` (RAM and scratchpad); false when the range is outside guest memory.
using GuestRead = bool (*)(std::uint32_t address, void *destination, std::size_t bytes) noexcept;

// The VU1 TOPS+0xC9 state a program run reads when its draw uploads nothing. The VU1 data memory is double buffered:
// VIF BASE/OFFSET select two 0xC9 locations and every MSCAL flips the buffer (`run_vu` in the runtime: `top = tops`,
// then `dbf = !dbf`, then `tops = base + (dbf ? ofst : 0)`), so a run reads the buffer written since the previous MSCAL.
// The capture therefore tracks one slot per parity of the observed MSCAL count. The slot labels are relative (the first
// observed run is slot 0): an upload and the read that follows it use the same parity, so a constant unknown offset
// cancels out. An *unseen* MSCAL (another call site of func_0049D308 writes outside these hooks, or a chain this capture
// could not parse) shifts the parity; `gap_epoch` marks that. A carried block is exact when its slot was uploaded after
// the last gap, or when both slots hold the same content (the ambiguity cannot matter); otherwise the item says so.
struct LightSlot {
  std::uint32_t block[kMaxLightBlockQwords][4] = {};
  std::uint32_t qwords = 0;   // 0 until the first upload into this slot
  std::uint32_t command = 0;
  std::uint32_t source = 0;
  std::uint32_t valid = 0;
  std::uint32_t epoch = 0;    // CarriedLight::gap_epoch when this block was uploaded
};

struct CarriedLight {
  LightSlot slot[2];
  std::uint32_t runs = 0;       // observed MSCALs; the next run reads slot[runs & 1]
  std::uint32_t gap_epoch = 0;  // bumped by the hook on a PGL output-cursor discontinuity or a failed chain
};

// One walked chain's accounting. The hook accumulates these into CaptureCounters; the fixture checks them.
struct ChainWalkCounters {
  std::uint32_t tags = 0;           // tag qwords seen
  std::uint32_t item_tags = 0;      // tags matched to the draw's item table, in order
  std::uint32_t light_tags = 0;     // uploads to TOPS+0xC9 (REF or CNT form)
  std::uint32_t other_ref_tags = 0; // REF tags that are neither an item nor a light upload
  std::uint32_t unknown_tags = 0;   // a tag id the PGL output chain does not use
  std::uint32_t items_uploaded = 0;
  std::uint32_t items_carried = 0;
  std::uint32_t items_uncertain = 0;
  std::uint32_t items_none = 0;
  std::uint32_t items_run = 0;          // items whose run pair was read and matched the VIF forms
  std::uint32_t items_run_missing = 0;  // items whose run pair was absent or not ITOP+MSCAL
  std::uint32_t item_variants = 0;      // items whose ITOP (the real program selector) differs from the terminating one
  std::uint32_t item_mscal_variants = 0;  // items whose MSCAL stub immediate differs from the terminating one
  bool complete = false;     // the walk parsed exactly from `begin` to `end`
  bool terminator = false;   // the terminating 0x10000000 tag was found at `end`
  bool item_mismatch = false;  // an item tag did not match the draw's item table in order
};

// Walks the PGL output chain [begin, end) in guest byte addresses.
//   * `draw` may be null: the walk then only tracks the carried light state (shape calls outside the car pass still
//     write chains, and their uploads are part of the VU1's carried state).
//   * The per-item light state and run pair are filled only for items whose tags were matched. When the chain is
//     complete and every item matched, items without an upload receive the carried state. Items left unmatched keep
//     light_source = kLightItemNone and run_valid = 0 (never guessed).
//   * The per-draw compatibility fields (light_block from item 0's upload, program_* from the terminating tag) are set
//     only when the corresponding tag was read.
// Returns true when the chain parsed exactly to `end` (an empty chain, begin == end, is complete).
bool walk_packet_chain(std::uint32_t begin, std::uint32_t end, GuestRead read, CapturedDraw *draw,
                       CarriedLight &carried, ChainWalkCounters &counters) noexcept;

}  // namespace gt4::coregs3
