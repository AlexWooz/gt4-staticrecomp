// TASK-160: the field of view adjustment switch, steps and per-target addresses (native/fov_adjust.hpp).
#include "fov_adjust.hpp"

#include "game_target.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::fov_adjust {

namespace {
bool g_enabled = false;
std::int32_t g_steps = 0;
}  // namespace

bool parse_switch(const char *value) {
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "1") == 0) return true;  // on by default
  if (std::strcmp(value, "0") == 0) return false;
  throw std::invalid_argument("GT4_FOV_ADJUST must be 0 or 1");
}

Addresses active_addresses() {
  const target::HookAddresses &hooks = target::active().hooks;
  return {hooks.fov_projection_call_return, hooks.fov_pad_buttons};
}

void configure_from_environment() {
  try {
    g_enabled = parse_switch(std::getenv(kSwitch));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[fov] %s, game not started\n", error.what());
    std::exit(1);
  }
  const Addresses addresses = active_addresses();
  if (g_enabled && !addresses.complete()) {
    target::note_hook_unavailable("field of view adjustment (GT4_FOV_ADJUST)");
    g_enabled = false;
  }
  g_steps = 0;
  if (!g_enabled)
    std::fprintf(stderr, "[fov] off\n");
  else
    std::fprintf(stderr,
                 "[fov] on: R3 + D-pad up/down zoom (0.5..2.0 in steps of 1/128 per VBlank), L3 + R3 resets (buttons 0x%08X, "
                 "projection call return 0x%08X)\n",
                 addresses.pad_buttons, addresses.projection_call_return);
}

bool enabled() { return g_enabled; }

std::int32_t current_steps() { return g_steps; }

void at_guest_vblank(const std::uint8_t *ram, std::uint32_t ram_size) {
  if (!g_enabled) return;
  static const Addresses addresses = active_addresses();
  if (addresses.pad_buttons + 2 > ram_size) return;
  const std::uint16_t halfword = static_cast<std::uint16_t>(ram[addresses.pad_buttons] | (ram[addresses.pad_buttons + 1] << 8));
  const std::int32_t next = next_steps(g_steps, static_cast<std::uint16_t>(~halfword));
  if (next != g_steps) {
    static int logged = 0;
    if (next == 0 || next == kMinSteps || next == kMaxSteps || logged < 4) {
      std::fprintf(stderr, "[fov] zoom %d/128 (k %.4f)\n", next, 1.0 + next / 128.0);
      ++logged;
    }
    g_steps = next;
  }
}

}  // namespace gt4::fov_adjust
