#pragma once

// TASK-213 Phase 1: the native GT4 `Wide` row as a host Fullscreen control.
//
// The Options > General > Screen page has one `OptionMenu` row with two choices (`Option4x3`/`Option16x9`). The
// recomp already holds 16:9 at every VBlank (native/race_widescreen.cpp), so the row is a no-op. A same-length data
// patch relabels it (label "Fullscreen", choices "Off"/"On", see docs/tasks/INGAME-SETTINGS-MAP.md section 11) and
// this module turns its script callback into a host control:
//
//   * the script's getter (`option.wide_mode`) returns the host value (0 Off, 1 On) while the Options project is the
//     current one, so `ScreenRoot.reflect` selects the row the host state says;
//   * the script's setter (`option.wide_mode = Pane::Wide::OptionMenu.index`) stores the host value and writes the
//     launcher feedback file; the guest option field (`option + 0x14`) and the 44-byte 16:9 record stay untouched;
//   * the window applies the value as borderless fullscreen at the next VBlank (the Alt+Enter path). Headless runs
//     only log it.
//
// The native callback is one entry (v2.00 0x00180108) for both directions: `argc > 0` is a set, `argc == 0` a get
// whose boxed `hInt` is written into the caller's return slot. `scripts/native_settings_overlay.inc` intercepts the
// dispatch of that entry and calls on_wide_mode(); when it returns true the native is skipped and the pc restored.
//
// Outside the Options project the callback is passed through unchanged: the arcade top menu reads `option.wide_mode`
// to place its 4:3/16:9 layout, and that must keep seeing the game's own 16:9 value.

#include <cstddef>
#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace gt4::native_settings {

// The environment variable the launcher sets to the feedback file path (same config folder as settings.toml).
inline constexpr const char *kFeedbackVariable = "GT4_SETTINGS_FEEDBACK";

// The v2.00 guest addresses of the contract (gt4::target::HookAddresses fills them; 0 = the target has none).
struct Guest {
  std::uint32_t wide_mode = 0;         // 0x00180108: MOption.wide_mode get/set
  std::uint32_t box_int = 0;           // 0x002FE278: box an int into an hValue slot
  std::uint32_t release_handle = 0;    // 0x003285F8: release a RefCounter handle
  std::uint32_t hint_vtable = 0;       // 0x00674188: the hInt vtable (the OptionMenu index object)
  std::uint32_t owner_pointer = 0;     // the word holding the game-data object (the option object's owner)
  std::uint32_t project_word = 0;      // owner + this = the next project's four-character id
  std::uint32_t option_project = 0;    // the id that means the Options project ("opti")
};

// The addresses of the active target, once (per-target values from game_target.hpp).
const Guest &guest();

// The dispatch address of the wide_mode callback, or 0 when this target has no native-settings contract.
std::uint32_t wide_mode_entry();

// The host state of the row: false = Off (windowed), true = On (fullscreen). Starts windowed until the window or the
// headless start reads GT4_FULLSCREEN.
bool host_fullscreen();
void set_host_fullscreen(bool on);

// A change the window has not applied yet. take() returns false when there is none and clears the request.
bool take_fullscreen_request(bool &on);

// TASK-213 Phase 1d: a live output-size request from the Screen rows. The window applies it at the next VBlank
// (windowed: the client area; fullscreen: the monitor's display mode). A headless run consumes nothing.
void request_output_size(int width, int height);
bool take_output_size_request(int &width, int &height);

// True while a game window exists (window.cpp sets it after creation and clears it before cleanup). The Screen rows use
// it to say whether a live apply is possible or the value only takes effect at the next start.
void set_window_present(bool present);
bool window_present();

// Records a change (`source` names the trigger, e.g. "Wide row" or "Alt+Enter"): stores the host value, writes the
// feedback file when GT4_SETTINGS_FEEDBACK is set, and logs one line. Returns false only when the file could not be
// written (the host value is stored either way).
bool record_fullscreen(bool on, const char *source);

// The dispatch entry point for the wide_mode callback. Returns true when this call was handled and the native must
// be skipped (the caller then returns immediately; pc was restored to the caller's return label). Returns false for a
// target without the addresses, outside the Options project, or for a value object this build does not understand.
bool on_wide_mode(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth);

// Writes the feedback file atomically: `version = 1` and `[changes] fullscreen = true|false`. The path comes from
// GT4_SETTINGS_FEEDBACK; an unset or empty variable writes nothing and returns false with a short reason.
bool write_feedback(bool fullscreen, char *error, std::size_t error_size);

// Writes the feedback file atomically with one or more `[changes]` entries (the value text is emitted verbatim, so a
// TOML boolean or a quoted string). Used by the Screen rows for `resolution`/`monitor`.
bool write_feedback_changes(const char *const *keys, const char *const *values, std::size_t count, char *error,
                            std::size_t error_size);

// True while the game's next project is the Options project (the page-gating rule the Wide row uses).
bool option_project_active(const std::uint8_t *ram, std::size_t size);

// Reads argv[0] as a guest hInt (the OptionMenu/SliderBar index). False for another object or a bad pointer.
bool read_hint_value(const std::uint8_t *ram, std::size_t size, std::uint32_t argv, std::int32_t &value);

// Test seam: replaces the guest boxing call (func_002FE278) so the fixture can check the returned value without the
// translated guest code. A null pointer restores the guest call.
using BoxInt = bool (*)(ps2::Cpu &cpu, std::uint32_t slot, std::int32_t value, std::uint32_t depth);
void set_box_int_for_test(BoxInt fn);

// Test seam: replaces the target address table, so the fixture pins the v2.00 contract in a Spec II build too.
void set_guest_for_test(const Guest &guest);

void reset_for_test();

} // namespace gt4::native_settings
