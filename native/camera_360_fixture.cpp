// CTest native_camera_360: the pure rules of the TASK-159 360-degree chase camera (native/camera_360.hpp), host only.
//   - GT4_CAMERA_360: unset, empty and 0 are off, 1 is on, anything else is refused;
//   - the stick byte to yaw offset table: 0 at rest, a half turn at both ends, odd around rest, monotonic, and the
//     reference patch's direction (right is positive);
//   - the yaw argument: unchanged at rest (no guest add at all), the guest add otherwise;
//   - the analog pedal return labels: only the target's two scale calls match;
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
  check(!parse_switch(nullptr), "unset is off");
  check(!parse_switch(""), "empty is off");
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
  check(v200.camera_accelerator_scale_return == 0x004387C8u, "v2.00 accelerator scale (func_004386F0 case 3, jal at 0x004387C0)");
  check(v200.camera_brake_scale_return == 0x004387F0u, "v2.00 brake scale (func_004386F0 case 4, jal at 0x004387E8)");
  check(active.accelerator_scale_return == v200.camera_accelerator_scale_return &&
            active.brake_scale_return == v200.camera_brake_scale_return && active.complete(),
        "the pedal hook reads the active (v2.00) table");
}

void check_pedal_returns() {
  constexpr Addresses addresses{0x003456D8u, 0x0086F99Eu, 0x004387C8u, 0x004387F0u};
  static_assert(is_pedal_scale_return(addresses, 0x004387C8u) && is_pedal_scale_return(addresses, 0x004387F0u));
  check(!is_pedal_scale_return(addresses, 0x00438818u), "case 5 (0x00438810) keeps its value");
  check(!is_pedal_scale_return(addresses, 0x003456D8u), "the yaw call is not a pedal call");
  check(!is_pedal_scale_return(Addresses{}, 0u), "an absent target matches nothing, not even ra 0");
  check(!Addresses{0x003456D8u, 0x0086F99Eu, 0u, 0u}.complete(), "a table without the pedal calls keeps the camera off");
}

}  // namespace

int main() {
  check_switch();
  check_offsets();
  check_yaw_argument();
  check_addresses();
  check_pedal_returns();
  if (failures != 0) {
    std::fprintf(stderr, "camera_360_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("camera_360_fixture: ok\n");
  return 0;
}
