#pragma once
// TASK-159: the 360-degree chase camera on the right analog stick (docs/CAMERA-360.md).
//
// Idea and call site from the 360 Degree Chase Camera patch by Vyerq/unko_ (v2.00) and Nenkai's port from Duchi/Vyerq
// (Spec II). This is our own implementation: no patch words are used.
//
// The game's chase camera follows the car with a damped spring on its yaw (v2.00 func_00345658, Spec II 0x002063D8):
// each frame it calls the angle-difference function (v2.00 func_0036CC40, Spec II 0x00230F00) with the target yaw in f12
// and the camera's current yaw in f13, and turns the result into a spring force. Adding an angle to f13 just before that
// call makes the spring settle that angle away from the target, so the camera swings round the car, smoothly, and swings
// back when the angle returns to 0. Nothing else is touched: the camera's own state stays the game's.
//
// The call is a direct `jal`, which the translator turns into a direct C++ call, so neither the runtime's dispatch nor a
// VBlank write can reach it. scripts/camera360_units.py therefore adds one call to before_chase_yaw_call() in the
// translated unit of that call site, between the delay slot (which loads f13) and the call; the EE library is then built
// from those units. With GT4_CAMERA_360 off, the function returns at once and the guest state is untouched.
//
// The angle comes from the right stick X byte of pad port 0 in the game's own pad buffer (v2.00 0x0086F99E, Spec II
// 0x0089B0DE: buffer + 0x1E), the byte the game itself reads, so a recorded drive replays the same camera.
// 128 is rest (no change at all); 0 is a half turn one way, 255 the half turn the other way, linear in between.
//
// The same stick's Y axis is the game's analog accelerator (up) and brake (down) in its default controller setup, so a
// sideways push that is not perfectly level would also accelerate or brake. While the camera is on, the race input's analog
// channel handler (v2.00 func_004386F0, Spec II 0x00314870) gets 0 as the value of its accelerator and brake cases, and its
// own zero test then stores nothing: the effect of the community "Disable right analog throttle/brake" patch, here only
// while GT4_CAMERA_360=1. The pressure buttons still accelerate and brake; steering and the menus are untouched.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace ps2 {
struct Cpu;
}

namespace gt4::camera360 {

inline constexpr const char *kSwitch = "GT4_CAMERA_360";
inline constexpr const char *kIniKey = "camera_360";

inline constexpr std::uint8_t kStickRest = 128;
inline constexpr std::uint32_t kPiBits = 0x40490FDBu;  // pi as the nearest f32

// GT4_CAMERA_360: nullptr (unset), "" or "0" = off; "1" = on. Any other value throws std::invalid_argument naming the
// variable, so a typo refuses the start instead of silently leaving the camera as it was.
bool parse_switch(const char *value);

// The yaw offset for a right stick X byte, as f32 bits: pi * clamp((stick - 128) / 127, -1, 1). 0 (no offset) at rest.
// Positive to the right, as the reference patch (which adds (stick - 127) * 0.0245 rad to f13).
constexpr std::uint32_t yaw_offset_bits(std::uint8_t stick_x) {
  const int deflection = static_cast<int>(stick_x) - kStickRest;
  if (deflection == 0) return 0;
  float share = static_cast<float>(deflection) / 127.0f;
  if (share > 1.0f) share = 1.0f;
  if (share < -1.0f) share = -1.0f;
  return std::bit_cast<std::uint32_t>(share * std::bit_cast<float>(kPiBits));
}

// The 256 offsets, computed once at compile time.
inline constexpr std::array<std::uint32_t, 256> kYawOffsetBits = [] {
  std::array<std::uint32_t, 256> table{};
  for (int stick = 0; stick < 256; ++stick) table[static_cast<std::size_t>(stick)] = yaw_offset_bits(static_cast<std::uint8_t>(stick));
  return table;
}();

// What the hooks need from one game target (native/game_target.hpp): the return labels of the calls (each hook checks that
// ra holds one of its own, so a unit of another target can never feed it) and the stick byte. Zeros mean "not in this
// target": the camera then stays off.
struct Addresses {
  std::uint32_t yaw_call_return = 0;
  std::uint32_t pad_stick_x = 0;
  std::uint32_t accelerator_scale_return = 0;
  std::uint32_t brake_scale_return = 0;
  constexpr bool complete() const {
    return yaw_call_return != 0 && pad_stick_x != 0 && accelerator_scale_return != 0 && brake_scale_return != 0;
  }
};
Addresses active_addresses();

// Reads GT4_CAMERA_360 once and prints one line when it is on. An invalid value prints the refusal and exits the process with
// status 1. Called at start (headless and windowed, launcher and direct starts); the hook reads the result.
void configure_from_environment();
bool enabled();

// The guest yaw argument after the offset: guest_add(yaw, offset) when the stick is off rest, else yaw unchanged.
// `guest_add` is the guest FPU add (ps2::fp::add in the hook), so the sum is the one the guest's own add.s would give.
template <typename GuestAdd>
std::uint32_t offset_yaw_argument(std::uint32_t yaw_bits, std::uint8_t stick_x, GuestAdd guest_add) {
  const std::uint32_t offset = kYawOffsetBits[stick_x];
  return offset == 0 ? yaw_bits : guest_add(yaw_bits, offset);
}

// True when a call with this return address is the analog accelerator or brake scale of the target.
constexpr bool is_pedal_scale_return(const Addresses &addresses, std::uint32_t return_address) {
  return return_address != 0 &&
         (return_address == addresses.accelerator_scale_return || return_address == addresses.brake_scale_return);
}

// Called by the translated call sites (scripts/camera360_units.py), after the delay slot and before the call.
// native/camera_360_hook.cpp; they read guest RAM and registers, so they live with the game executable, not in this pure
// library.
void before_chase_yaw_call(ps2::Cpu &c);     // adds the stick angle to f13
void before_analog_pedal_call(ps2::Cpu &c);  // sets the channel value a0 to 0

}  // namespace gt4::camera360
