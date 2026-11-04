// TASK-159: the calls that scripts/camera360_units.py adds to the translated game (native/camera_360.hpp).
//
// Each runs between the delay slot of a call and the call itself: in the chase camera, the yaw call (its delay slot or the
// code before it has just loaded the camera's current yaw into f13), exactly where the reference patch adds its angle; in the
// analog channel handler, the scale calls of the accelerator and brake cases (the delay slot has just copied the channel
// value into a0). Part of the game executable because they read guest RAM and registers; the rules they apply are the pure
// ones of camera_360.hpp.
#include "camera_360.hpp"

#include <ps2/cpu.hpp>
#include <ps2/fpu.hpp>
#include <ps2/memory.hpp>

#include <cstdio>

namespace gt4::camera360 {

void before_chase_yaw_call(ps2::Cpu &c) {
  if (!enabled()) return;
  static const Addresses addresses = active_addresses();
  const std::uint32_t return_address = c.r[31].w[0];
  if (return_address != addresses.yaw_call_return) {
    // Only the call site of this target's own units may feed the camera; anything else leaves the guest alone.
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[camera360] hook reached with ra 0x%08X, expected 0x%08X: camera unchanged\n", return_address,
                   addresses.yaw_call_return);
      logged = true;
    }
    return;
  }
  const std::uint8_t stick_x = ps2::g_ram[addresses.pad_stick_x];
  // f13 is the second float argument of the angle-difference call: the camera's current yaw.
  c.f[13] = offset_yaw_argument(c.f[13], stick_x, [](std::uint32_t a, std::uint32_t b) { return ps2::fp::add(a, b); });
}

void before_analog_pedal_call(ps2::Cpu &c) {
  if (!enabled()) return;
  static const Addresses addresses = active_addresses();
  const std::uint32_t return_address = c.r[31].w[0];
  if (!is_pedal_scale_return(addresses, return_address)) {
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[camera360] pedal hook reached with ra 0x%08X, expected 0x%08X or 0x%08X: input unchanged\n",
                   return_address, addresses.accelerator_scale_return, addresses.brake_scale_return);
      logged = true;
    }
    return;
  }
  // The scale of 0 is 0, and the handler's `beql a3,zero` then skips storing an accelerator or brake value.
  c.r[4].d[0] = 0;
}

}  // namespace gt4::camera360
