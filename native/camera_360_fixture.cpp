// CTest native_camera_360: the pure rules of the TASK-159 360-degree chase camera (native/camera_360.hpp), host only.
//   - GT4_CAMERA_360: unset, empty and 1 are on (the default, owner 2026-11-04), 0 is off, anything else is refused;
//   - the stick byte to yaw offset table: 0 at rest, a half turn at both ends, odd around rest, monotonic, and the
//     reference patch's direction (right is positive);
//   - the yaw argument: unchanged at rest (no guest add at all), the guest add otherwise;
//   - the analog channel rule: only a right stick Y channel assigned to the accelerator or the brake is cleared;
//   - the per-target addresses: v2.00 (the hook compiles against the active table; a Spec II build pins the fixture to v2.00).
#include "camera_360.hpp"
#include "game_target.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using namespace gt4::camera360;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "camera_360_fixture: FAILED %s\n", what);
    ++failures;
  }
}

float as_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }

void check_switch() {
  check(parse_switch(nullptr), "unset is on (the default)");
  check(parse_switch(""), "empty is on (the default)");
  check(!parse_switch("0"), "0 is off");
  check(parse_switch("1"), "1 is on");
  for (const char *value : {"2", "on", "yes", "-1", "01", " 1", "1 "}) {
    bool refused = false;
    try {
      parse_switch(value);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    if (!refused) std::fprintf(stderr, "camera_360_fixture: GT4_CAMERA_360=%s was accepted\n", value);
    check(refused, "an invalid switch value is refused");
  }
  check(std::string_view(kSwitch) == "GT4_CAMERA_360" && std::string_view(kIniKey) == "camera_360", "switch and ini key names");
}

void check_offsets() {
  const float pi = as_float(kPiBits);
  check(std::fabs(pi - 3.14159265f) < 1e-6f, "kPiBits is pi");
  check(kYawOffsetBits[kStickRest] == 0, "rest (128) is no offset");
  check(as_float(kYawOffsetBits[255]) == pi, "full right (255) is a half turn");
  check(as_float(kYawOffsetBits[0]) == -pi, "full left (0) is a half turn the other way (clamped)");
  check(as_float(kYawOffsetBits[1]) == -pi, "1 is the last step before the clamp: -127/127");
  check(kYawOffsetBits[129] == (kYawOffsetBits[127] ^ 0x80000000u), "one step either side of rest is the same angle, mirrored");
  check(as_float(kYawOffsetBits[129]) > 0.0f, "right of rest is positive (the reference patch's direction)");
  for (int stick = 1; stick < 256; ++stick) {
    const float previous = stick - 1 == kStickRest ? 0.0f : as_float(kYawOffsetBits[static_cast<std::size_t>(stick - 1)]);
    const float current = stick == kStickRest ? 0.0f : as_float(kYawOffsetBits[static_cast<std::size_t>(stick)]);
    if (stick != 1) check(current > previous, "the offset rises with the stick");
  }
  for (int deflection = 1; deflection <= 127; ++deflection) {
    const std::uint32_t right = kYawOffsetBits[static_cast<std::size_t>(kStickRest + deflection)];
    const std::uint32_t left = kYawOffsetBits[static_cast<std::size_t>(kStickRest - deflection)];
    check(right == (left ^ 0x80000000u), "the table is odd around rest");
  }
  // A quarter of the travel each way is close to a quarter turn (64/127 of pi).
  check(std::fabs(as_float(kYawOffsetBits[192]) - pi * 64.0f / 127.0f) < 1e-6f, "half deflection is about a quarter turn");
}

void check_yaw_argument() {
  int calls = 0;
  const auto counting_add = [&calls](std::uint32_t a, std::uint32_t b) {
    ++calls;
    return std::bit_cast<std::uint32_t>(std::bit_cast<float>(a) + std::bit_cast<float>(b));
  };
  const std::uint32_t yaw = std::bit_cast<std::uint32_t>(1.25f);
  check(offset_yaw_argument(yaw, kStickRest, counting_add) == yaw && calls == 0, "at rest the yaw is untouched (no add)");
  const std::uint32_t turned = offset_yaw_argument(yaw, 255, counting_add);
  check(calls == 1 && as_float(turned) == 1.25f + as_float(kPiBits), "off rest the yaw gets the offset through the guest add");
  // The add receives exactly the table entry as its second operand.
  std::uint32_t seen = 0;
  offset_yaw_argument(0u, 0, [&seen](std::uint32_t, std::uint32_t b) { seen = b; return b; });
  check(seen == kYawOffsetBits[0], "the add receives the table entry");
}

void check_addresses() {
  // A Spec II build compiles this fixture against the v2.00 table (GT4_TARGET_FIXTURE_V200), like the other hook fixtures.
  const gt4::target::HookAddresses &v200 = gt4::target::kV200Hooks;
  check(v200.camera_yaw_call_return == 0x003456D8u, "v2.00 yaw call return label (jal 0x0036CC40 at 0x003456D0)");
  check(v200.camera_pad_stick_x == 0x0086F99Eu, "v2.00 right stick X byte (pad buffer 0x0086F980 + 0x1E)");
  const Addresses active = active_addresses();
  check(active.yaw_call_return == v200.camera_yaw_call_return && active.pad_stick_x == v200.camera_pad_stick_x,
        "the hook reads the active (v2.00) table");
  check(v200.camera_pad_stick_x < gt4::target::kV200.ram_size, "the stick byte is inside guest RAM");
  check(v200.camera_analog_loop_return == 0x0043BE74u, "v2.00 analog loop call (func_0043BBC0, jal 0x004386F0 at 0x0043BE6C)");
  check(v200.camera_analog_channel_table == 0x006A64C0u && v200.camera_analog_slot_bias == 0u, "v2.00 slot table, slot in s0");
  check(active.analog_loop_return == v200.camera_analog_loop_return &&
            active.analog_channel_table == v200.camera_analog_channel_table && active.complete(),
        "the analog loop hook reads the active (v2.00) table");
}

void check_analog_channels() {
  // The game's slot table (both targets): LX, LY, RX, RY, then the half axes LX-, LX+, LY-, LY+, RX-, RX+, RY up, RY down.
  constexpr std::uint8_t slots[12] = {0x02, 0x03, 0x00, 0x01, 0x14, 0x15, 0x16, 0x17, 0x10, 0x11, 0x12, 0x13};
  static_assert(clears_analog_channel(0x12, kAnalogAccelerator) && clears_analog_channel(0x13, kAnalogBrake));
  for (std::uint8_t channel : slots) {
    const bool right_stick_y = channel == 0x01 || channel == 0x12 || channel == 0x13;
    for (std::int32_t function = -1; function <= 8; ++function) {
      const bool pedal = function == kAnalogAccelerator || function == kAnalogBrake;
      check(clears_analog_channel(channel, function) == (right_stick_y && pedal),
            "only a right stick Y channel assigned to the accelerator or the brake is cleared");
    }
  }
  check(!clears_analog_channel(0x16, kAnalogAccelerator) && !clears_analog_channel(0x17, kAnalogBrake),
        "the left stick Y halves keep the pedals");
  check(!clears_analog_channel(0x02, 0), "steering on the left stick X is untouched");
  check(!Addresses{0x003456D8u, 0x0086F99Eu, 0u, 0u, 0u}.complete(), "a table without the analog loop keeps the camera off");
}

}  // namespace

int main() {
  check_switch();
  check_offsets();
  check_yaw_argument();
  check_addresses();
  check_analog_channels();
  if (failures != 0) {
    std::fprintf(stderr, "camera_360_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("camera_360_fixture: ok\n");
  return 0;
}
