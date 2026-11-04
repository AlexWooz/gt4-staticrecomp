// CTest native_fov_adjust: the pure rules of the TASK-160 field of view adjustment (native/fov_adjust.hpp), host only.
//   - GT4_FOV_ADJUST: unset, empty and 1 are on (the default, owner 2026-11-04), 0 is off, anything else is refused;
//   - the steps: R3 + up raises, R3 + down lowers, L3 + R3 resets, nothing else moves them; other buttons may be held; the
//     range stops at 0.5 and 2.0 with no wrap;
//   - k: exactly 1.0 at 0 steps, 1 + steps/128 in exact f32;
//   - the per-target addresses (the v2.00 table; a Spec II build pins the fixture to v2.00).
#include "fov_adjust.hpp"
#include "game_target.hpp"

#include <bit>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using namespace gt4::fov_adjust;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "fov_adjust_fixture: FAILED %s\n", what);
    ++failures;
  }
}

void check_switch() {
  check(parse_switch(nullptr), "unset is on (the default)");
  check(parse_switch(""), "empty is on (the default)");
  check(parse_switch("1"), "1 is on");
  check(!parse_switch("0"), "0 is off");
  for (const char *value : {"2", "on", "yes", "-1", "01", " 1", "1 "}) {
    bool refused = false;
    try {
      parse_switch(value);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    check(refused, "an invalid switch value is refused");
  }
  check(std::string_view(kSwitch) == "GT4_FOV_ADJUST" && std::string_view(kIniKey) == "fov_adjust", "switch and ini key names");
}

void check_steps() {
  constexpr std::uint16_t cross = 1u << 14, r2 = 1u << 9, start = 1u << 3, left = 1u << 7, right = 1u << 5;
  check(next_steps(0, kR3 | kUp) == 1, "R3 + up raises by one step");
  check(next_steps(0, kR3 | kDown) == -1, "R3 + down lowers by one step");
  check(next_steps(37, kL3 | kR3) == 0 && next_steps(-12, kL3 | kR3 | kUp) == 0, "L3 + R3 resets, whatever else is held");
  check(next_steps(5, kUp) == 5 && next_steps(5, kDown) == 5 && next_steps(5, kR3) == 5, "up, down or R3 alone do nothing");
  check(next_steps(5, kL3 | kUp) == 5, "L3 + up does nothing");
  check(next_steps(5, kR3 | kUp | kDown) == 5, "R3 + up + down does nothing");
  check(next_steps(5, kR3 | kUp | cross | r2) == 6, "a held accelerator does not stop the zoom");
  check(next_steps(5, kR3 | kUp | left | right | start) == 6, "other D-pad directions and Start do not stop it either");
  check(next_steps(5, 0) == 5, "nothing pressed keeps the zoom");
  check(next_steps(kMaxSteps, kR3 | kUp) == kMaxSteps, "the top stops at 2.0 (no wrap)");
  check(next_steps(kMinSteps, kR3 | kDown) == kMinSteps, "the bottom stops at 0.5 (no wrap)");
  std::int32_t steps = 0;
  for (int vblank = 0; vblank < 1000; ++vblank) steps = next_steps(steps, kR3 | kUp);
  check(steps == kMaxSteps, "holding up for long ends at the top");
  for (int vblank = 0; vblank < 1000; ++vblank) steps = next_steps(steps, kR3 | kDown);
  check(steps == kMinSteps, "holding down for long ends at the bottom");
}

void check_factor() {
  check(factor_bits(0) == 0x3F800000u, "0 steps is exactly 1.0 (the game's own immediate)");
  check(std::bit_cast<float>(factor_bits(kMaxSteps)) == 2.0f, "the top is 2.0");
  check(std::bit_cast<float>(factor_bits(kMinSteps)) == 0.5f, "the bottom is 0.5");
  check(factor_bits(1) == 0x3F810000u, "one step up is the patch's first step (high half 0x3F81)");
  check(factor_bits(-1) == 0x3F7E0000u, "one step down is 1 - 1/128 (the patch's first step down is 1 - 1/256, 0x3F7F)");
  for (std::int32_t steps = kMinSteps; steps < kMaxSteps; ++steps)
    check(std::bit_cast<float>(factor_bits(steps + 1)) > std::bit_cast<float>(factor_bits(steps)), "k rises with the steps");
  for (std::int32_t steps = kMinSteps; steps <= kMaxSteps; ++steps)
    check(std::bit_cast<float>(factor_bits(steps)) == 1.0f + steps / 128.0f, "k is 1 + steps / 128 exactly");
}

void check_addresses() {
  const gt4::target::HookAddresses &v200 = gt4::target::kV200Hooks;
  check(v200.fov_projection_call_return == 0x0037A1C4u, "v2.00 projection store call (func_0037A170, jal 0x00378F18 at 0x0037A1BC)");
  check(v200.fov_pad_buttons == 0x0064C794u, "v2.00 port 0 button halfword");
  const Addresses active = active_addresses();
  check(active.projection_call_return == v200.fov_projection_call_return && active.pad_buttons == v200.fov_pad_buttons &&
            active.complete(),
        "the hook reads the active (v2.00) table");
  check(!Addresses{0x0037A1C4u, 0u}.complete() && !Addresses{0u, 0x0064C794u}.complete(), "a partial table keeps it off");
}

}  // namespace

int main() {
  check_switch();
  check_steps();
  check_factor();
  check_addresses();
  if (failures != 0) {
    std::fprintf(stderr, "fov_adjust_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("fov_adjust_fixture: ok\n");
  return 0;
}
