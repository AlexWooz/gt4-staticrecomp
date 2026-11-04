// TASK-159: the calls that scripts/camera360_units.py adds to the translated game (native/camera_360.hpp).
//
// Each runs between the delay slot of a call and the call itself: in the chase camera, the yaw call (its delay slot or the
// code before it has just loaded the camera's current yaw into f13), exactly where the reference patch adds its angle; in the
// race input's DualShock analog loop, its call to the channel handler (a2 = the function assigned to the slot, a3 = the
// channel value, s0 = the slot counter). Part of the game executable because they read guest RAM and registers; the rules
// they apply are the pure ones of camera_360.hpp.
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

void before_pad_analog_channel_call(ps2::Cpu &c) {
  if (!enabled()) return;
  static const Addresses addresses = active_addresses();
  const std::uint32_t return_address = c.r[31].w[0];
  if (return_address != addresses.analog_loop_return) {
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr, "[camera360] analog loop hook reached with ra 0x%08X, expected 0x%08X: input unchanged\n",
                   return_address, addresses.analog_loop_return);
      logged = true;
    }
    return;
  }
  const std::uint32_t slot = c.r[16].w[0] - addresses.analog_slot_bias;
  if (slot >= kAnalogSlots) return;
  const std::uint8_t channel = ps2::g_ram[addresses.analog_channel_table + slot];
  const std::int32_t function = static_cast<std::int32_t>(c.r[6].w[0]);
  // A value of 0 scales to 0, and the handler's `beql a3,zero` then skips storing an accelerator or brake value.
  if (clears_analog_channel(channel, function)) c.r[7].d[0] = 0;
}

}  // namespace gt4::camera360
