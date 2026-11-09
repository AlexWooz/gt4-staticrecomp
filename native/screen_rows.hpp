#pragma once

// TASK-213 Phase 1c: the Screen page's Resolution and Monitor rows as host controls.
//
// The two rows are the game's own `SliderBar`s (`Vertical` and `Horizontal`). Their values are painted by the page
// script (`setupSlider` writes `slider.index * bias` into the slider's value TextFace and popup), so a native override
// alone cannot keep a name there. This module runs at the main loop's render functor entry (after the page update,
// before the draw), writes the selected resolution/monitor name into the game's own `mTextFace` localized text, and
// sets the engine's insensitive bit while Fullscreen is Off. The two `MOption.screen_adjust_v/h` natives are
// intercepted: the getter returns the host list index, the setter stores it and writes `game-changes.toml`; the native
// is skipped, so the guest option fields and the save stay untouched.
//
// The lists come from Windows (`EnumDisplaySettingsW` for the current monitor's modes, `EnumDisplayMonitors` for the
// monitors); both are restart values (`GT4_OUTPUT_SIZE`/`GT4_MONITOR` are read once at start).

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

// `which` = 0 Resolution (screen_adjust_v), 1 Monitor (screen_adjust_h). Returns true when the call was handled and the
// native must be skipped (the caller returns; pc was restored to the return label).
bool on_adjust(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, int which);

// Per-frame service at the render functor entry: paints the value TextFaces and the insensitive state.
void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth);

// Reads GT4_OUTPUT_SIZE and GT4_MONITOR (the launcher's current run) and enumerates the modes and monitors once.
void configure_from_environment();

void reset_for_test();

} // namespace gt4::screen_rows
