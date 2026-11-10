#pragma once

// TASK-213 Phase 1d: the Screen page's Resolution and Monitor rows as the game's own OptionMenus.
//
// The stock `Vertical`/`Horizontal` SliderBar rows are hidden by the data patch (their script modules still run), and two
// scriptless `OptionMenu` rows are added to the Screen page: `Resolution` (the current monitor's modes, 1920x1080 first)
// and `Monitor` (the connected monitors). The engine owns the closed row, the popup, focus, scrolling and the controller;
// this module reads the selected index at the render functor entry, applies the value when the popup closes, writes
// `game-changes.toml`, and sets the engine's insensitive bit while Fullscreen is Off. The hidden sliders' two
// `MOption.screen_adjust_v/h` natives are still intercepted (getter 0, setter skipped) so the guest fields and the save
// stay untouched.
//
// A Resolution change is live:
//   * fullscreen: the window applies the monitor's display mode (ChangeDisplaySettingsExW) at the next VBlank and covers
//     the new mode; the original mode is restored when fullscreen is left or the game exits;
//   * windowed: the client area is resized (the row is greyed while Fullscreen is Off, the owner's rule);
//   * the aspect ratio follows the chosen size for the next race camera (display_aspect::set_live_output_size).
// A headless run has no window: the value is written to the feedback file and applies at the next start.

#include <cstddef>
#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace gt4::screen_rows {

// The dispatch entries, or 0 when this target has no Screen-rows contract (Spec II for now).
std::uint32_t adjust_v_entry();
std::uint32_t adjust_h_entry();
std::uint32_t render_entry();

// `which` = 0 Resolution (screen_adjust_v), 1 Monitor (screen_adjust_h). The stock sliders are hidden; the getter returns
// 0 and the setter is skipped, so `apply()` never writes the guest save. Returns true when the call was handled and the
// native must be skipped (pc was restored to the return label).
bool on_adjust(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, int which);

// Per-frame service at the render functor entry: finds the page and the two OptionMenu rows, syncs the current values,
// reads the index/open flag, applies a confirmed change and sets the insensitive state.
void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth);

// Reads GT4_OUTPUT_SIZE and GT4_MONITOR (the launcher's current run) and enumerates the modes and monitors once.
void configure_from_environment();

void reset_for_test();

} // namespace gt4::screen_rows
