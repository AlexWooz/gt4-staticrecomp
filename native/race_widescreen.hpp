#pragma once

// TASK-079: GT4_FORCE_WIDESCREEN=1 holds the game's own 16:9 setting ("Wide" on the Screen page) at guest VBlank.
//
// Guest chain (Confirmed from the decompiled source at identical addresses, gt4-decomp commit e0b70c4d6; see
// docs/BUILD.md slice211 for the sources of each address):
//   word at 0x622F4C          owner pointer (the game-data object; the race option is inside it)
//   owner + 0x38CB0           option object (MOption; setDefault = func_004363A0)
//   option + 0x14             wide flag, 0 = 4:3, 1 = 16:9 (= owner + 0x38CC4; set by func_00437110, read by
//                             func_001AB4C0 for the race HUD rectangle and by the photo mode code)
//   option + 0x24             screen aspect record, 11 words (func_00439108 copies one of four preset records into it)
//   option + 0x24 + 0xC/0x10  width and height as f32; func_004390D0 says "wide" when width / height >= 1.5 (f32)
//   option + 0x1388           video system, 1 = NTSC (func_00437110 passes (value == 1) as the NTSC argument)
//   presets (guest data)      0x622E90 (4:3), 0x622EC0 (wide), 0x622EF0 (4:3 NTSC), 0x622F20 (wide NTSC)
//                             index = wide + 2 * ntsc (func_00439108 selects the same way)
//   word at 0x6184F0          D_006184F0, the published "display is wide" word that the renderers read per frame
//                             (RaceDisplay, RaceDisplayUtil::wideOffset, the 0.75 scale in func_00393790 and
//                             func_00102B30). MOption.apply (func_004365B8) publishes it from the record.
//
// The host writes only guest RAM, the same way as GT4_FORCE_480P (race_display.*). It does the same work as the
// game's own setter (func_00437110 then func_004391C8): sets the flag, copies the wide preset when the record is not
// wide, and publishes the global. It never touches the owner's card file; a later game save stores what is in RAM.
// The game's menu can show 4:3 for one frame until the next VBlank forces it back (documented limit, like 480p).

#include <cstdint>

#include "game_target.hpp"

namespace gt4::display {

// TASK-100 S3: the numbers live in native/game_target.hpp (per-target table). These names are the v2.00 guest chain
// above, resolved for the active target; a target without the hook gets 0 and the hook disables itself.
inline constexpr std::uint32_t widescreen_owner_pointer_address =
    ::gt4::target::active().hooks.widescreen_owner_pointer;
inline constexpr std::uint32_t widescreen_option_object_offset =
    ::gt4::target::active().hooks.widescreen_option_offset;
inline constexpr std::uint32_t widescreen_flag_offset = ::gt4::target::active().hooks.widescreen_flag_offset;
inline constexpr std::uint32_t widescreen_record_offset = ::gt4::target::active().hooks.widescreen_record_offset;
inline constexpr std::uint32_t widescreen_record_bytes = ::gt4::target::active().hooks.widescreen_record_bytes;
inline constexpr std::uint32_t widescreen_record_width_offset =
    ::gt4::target::active().hooks.widescreen_record_width_offset;
inline constexpr std::uint32_t widescreen_record_height_offset =
    ::gt4::target::active().hooks.widescreen_record_height_offset;
inline constexpr std::uint32_t video_system_offset = ::gt4::target::active().hooks.video_system_offset;
inline constexpr std::uint32_t video_system_ntsc = ::gt4::target::active().hooks.video_system_ntsc;
inline constexpr std::uint32_t widescreen_global_address = ::gt4::target::active().hooks.widescreen_global;
inline constexpr std::uint32_t widescreen_preset_4_3 = ::gt4::target::active().hooks.widescreen_preset_4_3;
inline constexpr std::uint32_t widescreen_preset_wide = ::gt4::target::active().hooks.widescreen_preset_wide;
inline constexpr std::uint32_t widescreen_preset_4_3_ntsc =
    ::gt4::target::active().hooks.widescreen_preset_4_3_ntsc;
inline constexpr std::uint32_t widescreen_preset_wide_ntsc =
    ::gt4::target::active().hooks.widescreen_preset_wide_ntsc;

// GT4_FORCE_WIDESCREEN: unset or nullptr = 1 (on, the launcher default), "0" = off, "1" = on. Empty or any other
// value throws std::invalid_argument, so a typo refuses the start instead of silently changing the aspect.
bool parse_force_widescreen(const char *value);

// Guest address of the option object for an owner pointer, or 0 when the owner is null or the option fields
// (through the video system word) or the published global word are not inside guest RAM of `ram_size` bytes. Pure.
std::uint32_t widescreen_option_address(std::uint32_t owner_pointer, std::uint32_t ram_size);

// The game's own test (func_004390D0): width / height >= 1.5 in f32. `record` points at the 44-byte record. Pure.
bool record_is_widescreen(const std::uint8_t *record);

// Guest address of the preset record that func_00439108 copies for (wide, ntsc). Pure.
std::uint32_t widescreen_preset_address(bool wide, bool ntsc);

// TASK-080 hardening (gotchas/game-data-pointer-moves-at-boot, docs/ULTRAWIDE-NATIVE-STUDY.md section 3.4). At boot
// func_00108BF8 allocates one game-data object (func_0043A490, publishes it at 0x622F4C), loads data for a while, then
// func_0010AFC8 allocates the second one and publishes it: the pointer moves once (headless: 0x0096E360, then
// 0x00A0CC10). The hold must only write the live object, so it waits until the pointer has settled:
//   - the pointer is non-zero and unchanged for kOwnerSettleVblanks consecutive VBlanks, and
//   - it has moved at least once from one non-zero object to another (the boot object is never the live one), or,
//     if that move was never seen (both stores between two VBlanks), it has been unchanged for kOwnerLateSettleVblanks.
// A later move (none is known in this binary) starts the wait again. Memory card activity is not visible here (the
// card I/O lives in the memcard capsule); the settle wait keeps the first write after the boot load in practice.
inline constexpr std::uint32_t kOwnerSettleVblanks = 30;
inline constexpr std::uint32_t kOwnerLateSettleVblanks = 600;

struct OwnerSettle {
  std::uint32_t current = 0;  // owner pointer seen at the last VBlank
  std::uint32_t stable = 0;   // consecutive VBlanks with the same non-zero pointer
  std::uint32_t moves = 0;    // changes from one non-zero pointer to another
};

// One VBlank sample of the owner pointer. Returns true when `owner_pointer` is the settled live object. Pure.
bool owner_settled(OwnerSettle &settle, std::uint32_t owner_pointer);

struct WidescreenState {
  std::uint64_t writes = 0;
  bool logged = false;
  bool refused_logged = false;
  OwnerSettle settle;
  std::uint32_t logged_owner = 0;  // last owner pointer reported in the log
  bool settled_logged = false;
  std::uint64_t vblanks = 0;  // VBlanks seen by the hold (log lines only)
};

// GT4_FORCE_WIDESCREEN as read once per process: unset or "1" is on, "0" is off. An invalid value prints the
// refusal and exits the process with status 1 (called at window start and at the first guest VBlank).
bool widescreen_hold_enabled();

// Guest VBlank entry (mtvu_runtime.cpp vblank_sync, which the runtime calls at every guest VBlank in windowed and
// headless runs): holds the widescreen state when enabled, otherwise does nothing.
void widescreen_hold_at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size);

// One guest VBlank: when the owner chain is valid and the owner pointer has settled (OwnerSettle), forces the wide
// flag, the aspect record and the published global word in `ram`. Returns true when it wrote anything. A preset that
// is not wide (data not loaded) is refused: no write.
bool force_widescreen_at_vblank(std::uint8_t *ram, std::uint32_t ram_size, WidescreenState &state);

} // namespace gt4::display
