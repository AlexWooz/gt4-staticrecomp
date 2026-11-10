// TASK-090 CoreGS 3.0 track M1: the offline world pass's item selection rule.
//
// The selected picture is the sky (PassId 2), the far underlay (3) and the main-view course (5). Inside the course pass
// only the main-view ITOP 0x80 items are drawn: the measured main view composites through the identity program at VU1
// dispatch 0x0080 and its GS state carries XYOFFSET 0x6C08/0x7208. Every other outcome is an explicit, counted
// exclusion: the unmeasured 0xB08/0xA0/0x1278 programs, any other dispatch, an item with no position channel, an item
// with no tapped GS state, and the mirror/second-view items (0x7C08/0x7808 and 0x7C08/0x7C08). This unit keeps the rule
// in one reviewable place so the tool and its fixture cannot drift; it never guesses an item into the picture.
#pragma once

#include <cstdint>

#include "coregs3_scene.hpp"

namespace gt4::coregs3 {

enum class WorldExclusion : std::uint8_t {
  none = 0,
  not_world_pass,
  itop_b08,
  itop_a0,
  itop_1278,
  itop_other,
  no_position,
  no_state,
  mirror,
};
const char *world_exclusion_name(WorldExclusion exclusion) noexcept;

// The measured main-view GS XYOFFSET register word (OFX in the low 32 bits, OFY in the high 32).
inline constexpr std::uint64_t kMainViewXyOffset = 0x0000720800006C08ull;

struct WorldItemInput {
  std::uint32_t pass = 0;          // PassId
  std::uint32_t dispatch = 0;      // (run_itop & 0x3FF) * 8; 0 when the run pair is invalid
  bool run_valid = false;
  bool has_position = false;       // the shape scan found a descriptor position array
  bool state_known = false;        // the tap paired the item with a state record (TEST/ALPHA/FRAME/ZBUF/XYOFFSET)
  bool xyoffset_known = false;
  std::uint64_t xyoffset = 0;
  bool test_known = false;
  std::uint64_t test = 0;
  // The paired tapped record's TEST was 0 (ZTST NEVER): the "no-state" item the pass journal must not resurrect.
  bool test_zero = false;
};

// The selection of one captured item. `none` means the item is drawn; every other value is a counted exclusion.
WorldExclusion world_item_exclusion(const WorldItemInput &item) noexcept;

}  // namespace gt4::coregs3
