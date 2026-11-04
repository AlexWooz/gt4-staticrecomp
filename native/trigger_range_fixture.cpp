// CTest native_trigger_range: the TASK-161 full trigger range rules (native/trigger_range.hpp), host only.
//   - GT4_TRIGGER_FULL_RANGE: unset, empty and 1 are on (the default since the owner's rc14 test), 0 is off, anything else refused;
//   - off: the replacements give the original statements' values (2p, the game's divisor) and the game's own scale (100%
//     throttle from p = 45, brake from 84; 46 and 85 levels);
//   - on: no doubling and the divisor 99: 100% from p = 254 for both, a different level for every p up to 254.
#include "trigger_range.hpp"

#include <cstdio>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {

using namespace gt4::trigger_range;

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "trigger_range_fixture: FAILED %s\n", what);
    ++failures;
  }
}

void check_switch() {
  check(parse_switch(nullptr) && parse_switch("") && parse_switch("1"), "unset, empty and 1 are on (the default)");
  check(!parse_switch("0"), "0 is off");
  for (const char *value : {"2", "on", "true", "-1", "01", " 1"}) {
    bool refused = false;
    try {
      parse_switch(value);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    check(refused, "an invalid switch value is refused");
  }
  check(std::string_view(kSwitch) == "GT4_TRIGGER_FULL_RANGE" && std::string_view(kIniKey) == "trigger_full_range",
        "switch and ini key names");
}

int first_full(bool full_range, std::uint32_t divisor) {
  for (std::uint32_t p = 0; p < 256; ++p)
    if (pedal_level(full_range, p, divisor) == 256) return static_cast<int>(p);
  return -1;
}

std::size_t levels(bool full_range, std::uint32_t divisor) {
  std::set<std::uint32_t> seen;
  for (std::uint32_t p = 0; p < 256; ++p) seen.insert(pedal_level(full_range, p, divisor));
  return seen.size();
}

void check_scales() {
  // The game's own scale (v2.00 0x0043BEE8 and 0x0043BF48, Spec II 0x00319190 and 0x003191E8).
  check(first_full(false, 35) == 45 && first_full(false, 65) == 84, "off: 100% throttle from 45, brake from 84");
  check(pedal_level(false, 44, 35) == 251, "off: p = 44 gives 251/256 throttle");
  check(levels(false, 35) == 46 && levels(false, 65) == 85, "off: 46 throttle levels, 85 brake levels");
  // Full range: one scale for both.
  check(first_full(true, 35) == 254 && first_full(true, 65) == 254, "on: 100% from 254 for the throttle and the brake");
  check(levels(true, 35) == 255 && levels(true, 65) == 255, "on: 255 levels each (0..254 distinct, then 100%)");
  for (std::uint32_t p = 1; p < 255; ++p)
    check(pedal_level(true, p, 35) > pedal_level(true, p - 1, 35), "on: every step of the byte raises the throttle");
  check(pedal_level(true, 0, 35) == 0 && pedal_level(false, 0, 65) == 0, "nothing pressed is 0 either way");
}

void check_replacements() {
  set_enabled_for_tests(false);
  check(pedal_pressure(100) == 200 && pedal_pressure(200) == 400, "off: the doubling, as the original sll");
  check(pedal_divisor(35) == 35 && pedal_divisor(65) == 65 && pedal_divisor(20) == 20, "off: the game's divisor");
  set_enabled_for_tests(true);
  check(pedal_pressure(100) == 100 && pedal_pressure(255) == 255, "on: the pressure itself");
  check(pedal_divisor(35) == kFullRangeDivisor && pedal_divisor(65) == kFullRangeDivisor, "on: the full range divisor");
  set_enabled_for_tests(false);
}

}  // namespace

int main() {
  check_switch();
  check_scales();
  check_replacements();
  if (failures != 0) {
    std::fprintf(stderr, "trigger_range_fixture: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("trigger_range_fixture: ok\n");
  return 0;
}
