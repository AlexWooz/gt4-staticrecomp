// TASK-090 CoreGS 3.0: the PGL output packet chain walker (format version 4). See coregs3_chain.hpp for the structure
// and the evidence labels.
#include "coregs3_chain.hpp"

#include <cstring>

namespace gt4::coregs3 {
namespace {

constexpr std::uint32_t kTagBytes = 16u;
constexpr std::uint32_t kDmaIdCnt = 1u;
constexpr std::uint32_t kDmaIdRef = 3u;
constexpr std::uint32_t kTerminatorTag = 0x10000000u;  // CNT, QWC 0
constexpr std::uint32_t kLightDestination = 0xC9u;     // the TOPS+0xC9 light block

// A VIF UNPACK: opcode 0x60..0x7F in the top 7 bits.
bool is_unpack(std::uint32_t word) noexcept { return ((word >> 24) & 0x60u) == 0x60u; }

// The UNPACK writes `destination` in the TOPS window and its NUM is nonzero.
bool unpacks_to(std::uint32_t word, std::uint32_t destination) noexcept {
  if (!is_unpack(word)) return false;
  if (((word >> 15) & 1u) == 0) return false;
  if ((word & 0x3FFu) != destination) return false;
  return ((word >> 16) & 0xFFu) != 0;
}

// func_0049D328 writes word 2 as `0x04000000 | (obj->x + obj->y)` and word 3 as `0x14000000 | n`.
bool is_itop(std::uint32_t word) noexcept { return ((word >> 24) & 0x7Fu) == 0x04u; }
bool is_mscal(std::uint32_t word) noexcept { return ((word >> 24) & 0x7Fu) == 0x14u; }

void copy_block(std::uint32_t (&destination)[kMaxLightBlockQwords][4],
                const std::uint32_t (&source)[kMaxLightBlockQwords][4], std::uint32_t qwords) noexcept {
  std::memcpy(destination, source, static_cast<std::size_t>(qwords) * 16u);
  if (qwords < kMaxLightBlockQwords)
    std::memset(&destination[qwords][0], 0, static_cast<std::size_t>(kMaxLightBlockQwords - qwords) * 16u);
}

bool same_slot_content(const LightSlot &a, const LightSlot &b) noexcept {
  if (a.valid == 0 || b.valid == 0) return false;
  if (a.qwords != b.qwords || a.command != b.command) return false;
  return std::memcmp(a.block, b.block, static_cast<std::size_t>(a.qwords) * 16u) == 0;
}

// An upload between two MSCALs writes the buffer the next run reads: slot[carried.runs & 1].
void upload_slot(CarriedLight &carried, const std::uint32_t (&block)[kMaxLightBlockQwords][4],
                 std::uint32_t qwords, std::uint32_t command, std::uint32_t source) noexcept {
  LightSlot &slot = carried.slot[carried.runs & 1u];
  copy_block(slot.block, block, qwords);
  slot.qwords = qwords;
  slot.command = command;
  slot.source = source;
  slot.valid = 1;
  slot.epoch = carried.gap_epoch;
}

void set_item_upload(CapturedItem &item, const std::uint32_t (&block)[kMaxLightBlockQwords][4],
                     std::uint32_t qwords, std::uint32_t command, std::uint32_t source,
                     ChainWalkCounters &counters) noexcept {
  copy_block(item.light_block, block, qwords);
  item.light_block_qwords = qwords;
  item.light_block_command = command;
  item.light_block_source = source;
  if (item.light_source != kLightItemUploaded) counters.items_uploaded += 1;
  item.light_source = kLightItemUploaded;
}

// An item whose run has been reached uploads its own block or reads the carried one: this is the exact instant (all of
// the item's uploads precede the tag that runs it, and none of the next item's do). The carried block is exact when its
// slot was uploaded after the last parity gap, or when both slots hold the same content.
void finish_item(CapturedItem &item, const CarriedLight &carried, ChainWalkCounters &counters) noexcept {
  if (item.light_source != kLightItemNone) return;
  const LightSlot &slot = carried.slot[carried.runs & 1u];
  if (slot.valid == 0 || slot.qwords == 0) {
    counters.items_none += 1;
    return;
  }
  const bool exact = slot.epoch == carried.gap_epoch || same_slot_content(slot, carried.slot[(carried.runs + 1u) & 1u]);
  copy_block(item.light_block, slot.block, slot.qwords);
  item.light_block_qwords = slot.qwords;
  item.light_block_command = slot.command;
  item.light_block_source = slot.source;
  if (exact) {
    item.light_source = kLightItemCarried;
    counters.items_carried += 1;
  } else {
    item.light_source = kLightItemCarriedUncertain;
    counters.items_uncertain += 1;
  }
}

// One VIF MSCAL executes the microprogram of the last item tag seen (`current`); its tag2 pair is the run's selector.
void run_item(CapturedDraw *draw, std::uint32_t current, std::uint32_t itop, std::uint32_t mscal,
              CarriedLight &carried, ChainWalkCounters &counters) noexcept {
  if (draw != nullptr && current != kNoRecord && current < draw->item_count) {
    CapturedItem &item = draw->items[current];
    finish_item(item, carried, counters);
    item.run_itop = itop;
    item.run_mscal = mscal;
    if (is_itop(itop) && is_mscal(mscal)) {
      item.run_valid = 1;
      counters.items_run += 1;
    } else {
      item.run_valid = 0;
      counters.items_run_missing += 1;
    }
  }
  carried.runs += 1;
}

// The terminating tag carries the last item's pair; this is also the per-draw selector record (format version 3).
void set_draw_program(CapturedDraw &draw, std::uint32_t itop, std::uint32_t mscal) noexcept {
  if (!is_itop(itop) || !is_mscal(mscal)) return;
  draw.program_tag = mscal;
  draw.program_selector = mscal & 0xFFFFu;
  draw.program_entry = draw.program_selector * 8u;
  draw.program_itop = itop;
  draw.program_dispatch = (itop & 0x3FFu) * 8u;
  draw.program_count = draw.pgl[5];  // PGL +0x14, the MSCAL selector's source
  draw.program_valid = 1;
}

}  // namespace

bool walk_packet_chain(std::uint32_t begin, std::uint32_t end, GuestRead read, CapturedDraw *draw,
                       CarriedLight &carried, ChainWalkCounters &counters) noexcept {
  counters = ChainWalkCounters{};
  if (read == nullptr) return false;
  if (begin == end) {
    // func_0049D328 returns before writing anything when the Cmd or obj->count is zero.
    counters.complete = true;
    if (draw != nullptr) {
      draw->chain_valid = 1;
      counters.item_mismatch = draw->item_count != 0;
    }
    return true;
  }
  if (end < begin || (end - begin) % kTagBytes != 0) return false;

  const std::uint32_t item_count = draw != nullptr ? draw->item_count : 0u;
  std::uint32_t matched = 0;
  std::uint32_t current_item = kNoRecord;
  std::uint32_t position = begin;
  while (position < end) {
    std::uint32_t tag[4] = {};
    if (!read(position, tag, sizeof tag)) break;
    counters.tags += 1;
    const std::uint32_t qwc = tag[0] & 0xFFFFu;
    const std::uint32_t id = (tag[0] >> 28) & 7u;
    const std::uint32_t command = tag[3];
    const std::uint32_t num = (command >> 16) & 0xFFu;
    const bool is_last = (end - position) == kTagBytes;
    const bool light_upload =
        unpacks_to(command, kLightDestination) && num == qwc && qwc != 0 && qwc <= kMaxLightBlockQwords;

    // The tag's VIFcode pair executes before the tag's own data. A MSCAL is a real VU1 run: it reads the buffer written
    // since the previous run (the slot of the current MSCAL parity) and flips the double buffer for the next one.
    if (is_mscal(command)) {
      run_item(draw, current_item, tag[2], tag[3], carried, counters);
      current_item = kNoRecord;
    }

    if (id == kDmaIdCnt) {
      if (tag[0] == kTerminatorTag && qwc == 0) {
        if (!is_last) break;  // the terminator is the last qword func_0049D328 writes
        counters.terminator = true;
        if (draw != nullptr) set_draw_program(*draw, tag[2], tag[3]);
        position = end;
        break;
      }
      if ((static_cast<std::uint64_t>(qwc) + 1u) * kTagBytes > end - position) break;
      if (light_upload) {
        const std::uint32_t source = position + kTagBytes;
        std::uint32_t block[kMaxLightBlockQwords][4] = {};
        if (!read(source, block, qwc * 16u)) break;
        upload_slot(carried, block, qwc, command, source);
        counters.light_tags += 1;
        if (draw != nullptr && current_item != kNoRecord && current_item < item_count)
          set_item_upload(draw->items[current_item], block, qwc, command, source, counters);
      }
      position += (static_cast<std::uint32_t>(qwc) + 1u) * kTagBytes;
      continue;
    }

    if (id == kDmaIdRef) {
      // An item tag's tag2 word is STMASK data or the ITOP/MSCAL pair, never a light UNPACK, so the light test is the
      // more specific one and runs first: a light source address that happens to equal the next item's address cannot
      // be mistaken for that item's tag.
      if (light_upload) {
        const std::uint32_t source = tag[1] & 0x0FFFFFFFu;
        std::uint32_t block[kMaxLightBlockQwords][4] = {};
        if (!read(source, block, qwc * 16u)) break;
        upload_slot(carried, block, qwc, command, source);
        counters.light_tags += 1;
        if (draw != nullptr && current_item != kNoRecord && current_item < item_count)
          set_item_upload(draw->items[current_item], block, qwc, command, source, counters);
        position += kTagBytes;
        continue;
      }
      const bool item_match = draw != nullptr && matched < item_count &&
                              tag[1] == (draw->items[matched].address & 0x0FFFFFFFu) &&
                              qwc == draw->items[matched].qwords;
      if (item_match) {
        // A run that did not execute (no MSCAL in the tag2 pair) still ends the previous item's light state.
        if (draw != nullptr && current_item != kNoRecord && current_item < item_count)
          finish_item(draw->items[current_item], carried, counters);
        current_item = matched;
        matched += 1;
        counters.item_tags += 1;
        position += kTagBytes;
        continue;
      }
      counters.other_ref_tags += 1;
      position += kTagBytes;
      continue;
    }

    // Ids 0, 2, 4, 5, 6 and 7 are not part of the PGL output chain func_0049D328 writes.
    counters.unknown_tags += 1;
    break;
  }

  // An item whose run never executed (a chain without a terminating tag) still has its light state resolved, without a
  // run pair: the renderer can tell it never ran from run_valid.
  if (draw != nullptr && current_item != kNoRecord && current_item < draw->item_count)
    finish_item(draw->items[current_item], carried, counters);

  counters.complete = position == end;
  if (draw == nullptr) return counters.complete;

  draw->chain_tags = counters.tags;
  counters.item_mismatch = matched != item_count || (!counters.complete) || (!counters.terminator);
  draw->chain_valid = counters.item_mismatch ? 0u : 1u;

  // The per-draw compatibility fields keep their version 2/3 meaning: the first item's upload, the terminating pair.
  if (matched > 0 && draw->items[0].light_source == kLightItemUploaded) {
    draw->light_block_qwords = draw->items[0].light_block_qwords;
    draw->light_block_command = draw->items[0].light_block_command;
    draw->light_block_source = draw->items[0].light_block_source;
    copy_block(draw->light_block, draw->items[0].light_block, draw->light_block_qwords);
  }

  // The per-item selector evidence: the ITOP dispatch is the real program selector (round 13); the MSCAL word is a
  // shared stub whose immediate can differ (the decomp's per-item counter and 0xD9 arithmetic). `program_uniform`
  // therefore means the ITOP dispatch is the same for every item; the MSCAL difference is counted separately.
  if (draw->program_valid != 0 && draw->item_count != 0 && counters.items_run == draw->item_count &&
      counters.items_run_missing == 0) {
    std::uint32_t variants = 0, mscal_variants = 0;
    for (std::uint32_t index = 0; index < draw->item_count; ++index) {
      const CapturedItem &item = draw->items[index];
      if (item.run_itop != draw->program_itop) variants += 1;
      if (item.run_mscal != draw->program_tag) mscal_variants += 1;
    }
    counters.item_variants = variants;
    counters.item_mscal_variants = mscal_variants;
    draw->program_uniform = variants == 0 ? 1u : 0u;
  }
  return counters.complete;
}

}  // namespace gt4::coregs3
