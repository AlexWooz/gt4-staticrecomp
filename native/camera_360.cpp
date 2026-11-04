// TASK-159: the 360-degree chase camera switch and per-target addresses (native/camera_360.hpp).
#include "camera_360.hpp"

#include "game_target.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::camera360 {

namespace {
bool g_enabled = false;
}

bool parse_switch(const char *value) {
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) return false;
  if (std::strcmp(value, "1") == 0) return true;
  throw std::invalid_argument("GT4_CAMERA_360 must be 0 or 1");
}

Addresses active_addresses() {
  const target::HookAddresses &hooks = target::active().hooks;
  return {hooks.camera_yaw_call_return, hooks.camera_pad_stick_x, hooks.camera_accelerator_scale_return,
          hooks.camera_brake_scale_return};
}

void configure_from_environment() {
  try {
    g_enabled = parse_switch(std::getenv(kSwitch));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[camera360] %s, game not started\n", error.what());
    std::exit(1);
  }
  const Addresses addresses = active_addresses();
  if (g_enabled && !addresses.complete()) {
    target::note_hook_unavailable("360-degree chase camera (GT4_CAMERA_360)");
    g_enabled = false;
  }
  if (g_enabled)
    std::fprintf(stderr,
                 "[camera360] on: right stick X (guest byte 0x%08X) turns the chase camera (call return 0x%08X); analog "
                 "accelerator and brake off (returns 0x%08X, 0x%08X)\n",
                 addresses.pad_stick_x, addresses.yaw_call_return, addresses.accelerator_scale_return,
                 addresses.brake_scale_return);
}

bool enabled() { return g_enabled; }

}  // namespace gt4::camera360
