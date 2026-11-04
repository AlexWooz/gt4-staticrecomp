// TASK-161: the full trigger range switch and the translated statements' replacements (native/trigger_range.hpp).
#include "trigger_range.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::trigger_range {

namespace {
bool g_enabled = false;
// GT4_TRIGGER_TRACE=1 (diagnostic): one line per change of a pedal's level, from the pressure the case read and the divisor
// it divides by (35 throttle, 65 brake), so a recorded trigger sweep shows the levels the car receives.
bool g_trace = false;
std::uint32_t g_last_pressure = 0;      // the pressure of the case being computed (pedal_pressure runs first)
std::uint32_t g_last_level[2] = {~0u, ~0u};  // per pedal: throttle, brake
}  // namespace

bool parse_switch(const char *value) {
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) return false;  // off by default (owner)
  if (std::strcmp(value, "1") == 0) return true;
  throw std::invalid_argument("GT4_TRIGGER_FULL_RANGE must be 0 or 1");
}

void configure_from_environment() {
  try {
    g_enabled = parse_switch(std::getenv(kSwitch));
  } catch (const std::invalid_argument &error) {
    std::fprintf(stderr, "[trigger] %s, game not started\n", error.what());
    std::exit(1);
  }
  g_trace = std::getenv("GT4_TRIGGER_TRACE") != nullptr;
  if (g_enabled)
    std::fprintf(stderr, "[trigger] full range on: throttle and brake from the whole pressure byte (100%% from %u)\n",
                 (256 * kFullRangeDivisor + 99) / 100);
  else
    std::fprintf(stderr, "[trigger] full range off: the game's own throttle and brake scales\n");
}

bool enabled() { return g_enabled; }

void set_enabled_for_tests(bool on) { g_enabled = on; }

std::uint32_t pedal_pressure(std::uint32_t value) {
  g_last_pressure = value;
  return pedal_pressure_for(g_enabled, value);
}

std::uint32_t pedal_divisor(std::uint32_t divisor) {
  if (g_trace && (divisor == 35 || divisor == 65)) {
    const int pedal = divisor == 35 ? 0 : 1;
    const std::uint32_t level = pedal_level(g_enabled, g_last_pressure, divisor);
    if (level != g_last_level[pedal]) {
      std::fprintf(stderr, "[trigger] %s pressure %u level %u/256\n", pedal == 0 ? "throttle" : "brake", g_last_pressure,
                   level);
      g_last_level[pedal] = level;
    }
  }
  return pedal_divisor_for(g_enabled, divisor);
}

}  // namespace gt4::trigger_range
