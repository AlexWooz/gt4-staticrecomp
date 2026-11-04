#pragma once

// Slice164: GT4_FORCE_480P=1 holds the game's own race display option at 480p progressive.
//
// Guest chain (Confirmed from the pinned SCUS_973.28 ELF, see docs/BUILD.md slice164):
//   word at 0x622F4C          owner pointer (set by func_0043A490; read by func_001103E0)
//   owner + 0x38CB0           race option object (passed to func_00436740 and func_00436640)
//   object + 0x16B0           race display index: 0 = normal (480i), 1 = 480p, 2 = 1080i
// Index names come from the guest table at 0x622DF0 (func_00437380, func_004373F0).
// func_00436640 reads the index at race start and selects the 480p setting for index 1.
// The game stores 0 at 0x16B0 in func_004363A0 (setDefault).
//
// The host writes 1 at guest VBlank when the index is 0 (normal) or 2 (1080i); index 1 is left as it is.
// TASK-078 (owner, 2026-11-03; idea from PR #174): the launcher always asks for 480p, and a save or an imported card that
// holds 1080i raced at 1080i. The write is a guest RAM store, not a SetGsCrt rewrite.

#include <cstdint>

#include "game_target.hpp"

namespace gt4::display {

// TASK-100 S3: the numbers live in native/game_target.hpp (per-target table); see race_widescreen.hpp.
inline constexpr std::uint32_t race_owner_pointer_address = ::gt4::target::active().hooks.widescreen_owner_pointer;
inline constexpr std::uint32_t race_option_object_offset = ::gt4::target::active().hooks.widescreen_option_offset;
inline constexpr std::uint32_t race_display_index_offset = ::gt4::target::active().hooks.race_display_index_offset;
inline constexpr std::uint32_t race_display_index_normal =
    ::gt4::target::active().hooks.race_display_index_normal;
inline constexpr std::uint32_t race_display_index_480p = ::gt4::target::active().hooks.race_display_index_480p;
inline constexpr std::uint32_t race_display_index_1080i = ::gt4::target::active().hooks.race_display_index_1080i;

// GT4_FORCE_480P: nullptr or "0" is off, "1" is on. Any other value throws std::invalid_argument.
bool parse_force_480p(const char *value);

// Guest address of the race display index for an owner pointer, or 0 when the pointer or the
// index word is not inside guest RAM of `ram_size` bytes. Pure.
std::uint32_t race_display_index_address(std::uint32_t owner_pointer, std::uint32_t ram_size);

// True when the index word should be forced to 480p: the normal and the 1080i values. Pure.
bool should_force_480p_index(std::uint32_t index_value);

struct Force480pState {
  std::uint64_t writes = 0;
  bool logged = false;
};

// One guest VBlank: reads the owner pointer and the index from `ram`, writes 480p when the index
// is normal, updates `state` and logs the first write. Returns true when it wrote.
bool force_race_480p_at_vblank(std::uint8_t *ram, std::uint32_t ram_size, Force480pState &state);

} // namespace gt4::display
