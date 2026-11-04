#pragma once
// TASK-161: full trigger range for the throttle and the brake (docs/research/TRIGGER-SENSITIVITY.md).
//
// Idea from the "Full Trigger Sensitivity" PCSX2 patch by Silent & Aero_ (both games); our own implementation, no patch words.
//
// The race input turns the pressure byte p (0..255) of the button assigned to the throttle or the brake into
// min(100 * min(2p, 256) / d, 256) << 4, with d = 35 for the throttle and 65 for the brake (v2.00 func_0043BBC0 cases at
// 0x0043BEE8 and 0x0043BF48, Spec II 0x00319190 and 0x003191E8): 100% from p = 45 and p = 84, so only 46 and 85 of the
// byte's 256 values give different throttle and brake levels. With the switch on the doubling is left out and d is 99:
// min(100p / 99, 256), 100% from p = 254, about 255 levels for both. scripts/camera360_units.py replaces exactly two
// statements per case in the translated units: the doubling `v1 << 1` becomes pedal_pressure(v1) and the divide's divisor
// becomes pedal_divisor(d); with the switch off both return what the original statements computed (2v, d).
//
// The host's trigger default ranges follow the same switch (native/controls.cpp): R2 0..45 and L2 0..84 with it off (the stock
// game's 100%, TASK-147), the whole byte with it on, so the end of the trigger's travel is the game's 100% either way.
//
// GT4_TRIGGER_FULL_RANGE: unset, empty or 0 = off (the default until the owner's in-game test); 1 = on; anything else refuses
// the start. game.ini key trigger_full_range (the launcher's toggle).

#include <cstdint>

namespace gt4::trigger_range {

inline constexpr const char *kSwitch = "GT4_TRIGGER_FULL_RANGE";
inline constexpr const char *kIniKey = "trigger_full_range";
inline constexpr std::uint32_t kFullRangeDivisor = 99;  // 100 * 254 / 99 = 256: 100% from p = 254 (the patch: 98, from 251)

bool parse_switch(const char *value);

// Reads GT4_TRIGGER_FULL_RANGE once (start); an invalid value prints the refusal and exits with status 1. Prints one line.
void configure_from_environment();
bool enabled();
// For fixtures and the controls: sets the state as if configured.
void set_enabled_for_tests(bool on);

// The translated statements' replacements (scripts/camera360_units.py). Off: the original computation.
constexpr std::uint32_t pedal_pressure_for(bool full_range, std::uint32_t value) { return full_range ? value : value << 1; }
constexpr std::uint32_t pedal_divisor_for(bool full_range, std::uint32_t divisor) {
  return full_range ? kFullRangeDivisor : divisor;
}
std::uint32_t pedal_pressure(std::uint32_t value);
std::uint32_t pedal_divisor(std::uint32_t divisor);

// The game's pedal value (before the << 4) for a pressure byte, as the translated case computes it (for fixtures and docs).
constexpr std::uint32_t pedal_level(bool full_range, std::uint32_t pressure, std::uint32_t stock_divisor) {
  std::uint32_t v = pedal_pressure_for(full_range, pressure & 0xFFu);
  if (!(v < 256)) v = 256;
  const std::uint32_t w = (v * 100) / pedal_divisor_for(full_range, stock_divisor);
  return w < 256 ? w : 256;
}

}  // namespace gt4::trigger_range
