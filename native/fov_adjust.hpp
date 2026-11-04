#pragma once
// TASK-160: field of view adjustment from the pad (docs/FOV-ADJUST.md).
//
// Idea from the "FoV Adjustment" PCSX2 patch by Duchi/Vyerq (v2.00) and Blitznoz (Spec II); our own implementation, no
// patch words are used.
//
// The race cameras set their projection in one function (v2.00 func_0037A170, Spec II 0x0023E7E0; gt4-decomp 3b1f2ae5b):
// t = tan(fov / 2) from the camera's field of view in degrees (camera + 0xC), then func_00378F18 (Spec II 0x0023D558) stores
// the projection scales x = 1, y = 1 and depth = 1 / t at camera + 0x1C, 0x20, 0x24. The 1 is an immediate (`lui at,0x3F80`),
// which the patch rewrites in guest code at run time. Translated code cannot be rewritten that way, so
// scripts/camera360_units.py adds a host call between the delay slot of that store call and the call:
// before_projection_scale_call() gives it x = y = k and depth = k / t (the guest's own div.s), exactly what the patched
// immediate gives, with k the host's zoom factor. k = 1 changes nothing at all.
//
// k follows the pad as the game sees it: the port 0 button halfword the game keeps (v2.00 0x0064C794, Spec II 0x00681344,
// active low), read once per guest VBlank, so a recorded drive replays the same zoom. R3 + D-pad up raises k by one step per
// VBlank, R3 + D-pad down lowers it, L3 + R3 sets it back to 1. Other buttons may be held at the same time (a trigger on the
// accelerator, for instance). The step and the range follow what the patch gives in practice: it adds 1 to the low byte of
// the immediate's high half every frame, which is 1/128 above 1.0 (1/256 below), and the byte wraps after 127 steps up
// (1.9921875 to 0.5) or 128 down (0.5 to 1.9921875). Here: k = 1 + n / 128 with n from -64 to +128, so k runs from 0.5 to
// 2.0 in exact f32 steps of 1/128 and stops at the ends.

#include <bit>
#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace gt4::fov_adjust {

inline constexpr const char *kSwitch = "GT4_FOV_ADJUST";
inline constexpr const char *kIniKey = "fov_adjust";

// GT4_FOV_ADJUST: nullptr (unset), "" or "1" = on (the default, owner 2026-11-04); "0" = off. Any other value throws
// std::invalid_argument naming the variable.
bool parse_switch(const char *value);

// The DualShock 2 button bits of the halfword (the game's order; the halfword is active low).
inline constexpr std::uint16_t kL3 = 1u << 1;
inline constexpr std::uint16_t kR3 = 1u << 2;
inline constexpr std::uint16_t kUp = 1u << 4;
inline constexpr std::uint16_t kDown = 1u << 6;

inline constexpr std::int32_t kStepsPerUnit = 128;  // k = 1 + steps / 128
inline constexpr std::int32_t kMinSteps = -64;      // k = 0.5
inline constexpr std::int32_t kMaxSteps = 128;      // k = 2.0

// One guest VBlank: the steps after the pad's buttons (`pressed` = the bits held, i.e. the inverted halfword).
constexpr std::int32_t next_steps(std::int32_t steps, std::uint16_t pressed) {
  if ((pressed & kR3) == 0) return steps;
  if ((pressed & kL3) != 0) return 0;
  const bool up = (pressed & kUp) != 0, down = (pressed & kDown) != 0;
  if (up == down) return steps;
  const std::int32_t next = steps + (up ? 1 : -1);
  return next < kMinSteps ? kMinSteps : next > kMaxSteps ? kMaxSteps : next;
}

// k as f32 bits; exactly 1.0 (0x3F800000) at 0 steps.
constexpr std::uint32_t factor_bits(std::int32_t steps) {
  return std::bit_cast<std::uint32_t>(1.0f + static_cast<float>(steps) / static_cast<float>(kStepsPerUnit));
}

struct Addresses {
  std::uint32_t projection_call_return = 0;  // return label of the projection store call in the projection function
  std::uint32_t pad_buttons = 0;             // port 0 button halfword the game keeps (active low)
  constexpr bool complete() const { return projection_call_return != 0 && pad_buttons != 0; }
};
Addresses active_addresses();

// Reads GT4_FOV_ADJUST once and prints one line; an invalid value prints the refusal and exits with status 1.
void configure_from_environment();
bool enabled();
// The current steps (0 = k 1.0); for the logs and the hook.
std::int32_t current_steps();

// Guest VBlank (mtvu_runtime.cpp vblank_sync): reads the button halfword and steps k. Does nothing when off.
void at_guest_vblank(const std::uint8_t *ram, std::uint32_t ram_size);

// Called by the translated projection function (scripts/camera360_units.py), after the delay slot and before the call
// that stores the scales (native/fov_adjust_hook.cpp).
void before_projection_scale_call(ps2::Cpu &c);

}  // namespace gt4::fov_adjust
