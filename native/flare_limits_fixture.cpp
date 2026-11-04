// CTest native_flare_limits: the TASK-117 parameter block (native/flare_limits.hpp), host only.
//   - the rc6 words and the GT4_FLARE_LIMITS switch (0 = a zero block, 1 = the limits, anything else refused);
//   - the write into guest RAM at 0x7F10: exactly six words, idempotent, and it never touches the TASK-080 HUD block;
//   - the VBlank wrapper with the switch on.
#include "flare_limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace gt4::flare_limits;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void check_rc6_words() {
  const Parameters parameters = rc6_parameters();
  require(parameters.words[0] == 7, "flags are no halo + distance cull + light caps");
  require(parameters.words[1] == 0x42700000u, "track depth is 60.0f");
  require(parameters.words[2] == 0x42200000u, "car depth is 40.0f");
  require(parameters.words[3] == 0, "the reserved word is zero");
  require(parameters.words[4] == 12 && parameters.words[5] == 12, "both caps are 12");
  require(kParameterBlock == 0x7F10u && kParameterWords == 6, "the block follows the HUD block");
  require(kParameterBlock + kParameterWords * 4 <= 0x7F28u, "the block stays below 0x7F28");
}

void check_switch() {
  require(parameters_from_switch(nullptr) == Parameters{}, "unset is the original behaviour");
  require(parameters_from_switch("") == Parameters{}, "empty is the original behaviour");
  require(parameters_from_switch("0") == Parameters{}, "0 is the original behaviour");
  require(parameters_from_switch("1") == rc6_parameters(), "1 is the rc6 limits");
  for (const char *value : {"2", "yes", "-1", "01"}) {
    bool refused = false;
    try {
      parameters_from_switch(value);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    if (!refused) {
      std::fprintf(stderr, "[flare-limits] GT4_FLARE_LIMITS=%s was accepted\n", value);
      throw std::runtime_error("an invalid switch value must be refused");
    }
  }
}

void check_write() {
  std::vector<std::uint8_t> ram(32u << 20, 0);
  // Guards: the TASK-080 HUD block just below (0x7F00..0x7F07) and a word just above the block (0x7F28).
  for (std::uint32_t index = 0; index < 2; ++index) {
    const std::uint32_t word = 0x11223344u + index;
    std::memcpy(ram.data() + 0x7F00u + index * 4, &word, sizeof word);
  }
  const std::uint32_t above = 0x55667788u;
  std::memcpy(ram.data() + kParameterBlock + kParameterWords * 4, &above, sizeof above);
  require(write_parameters(ram.data(), static_cast<std::uint32_t>(ram.size()), rc6_parameters()), "the first write");
  for (std::uint32_t index = 0; index < kParameterWords; ++index) {
    std::uint32_t word = 0;
    std::memcpy(&word, ram.data() + kParameterBlock + index * 4, sizeof word);
    if (word != rc6_parameters().words[index]) {
      std::fprintf(stderr, "[flare-limits] word %u is 0x%08X\n", index, word);
      throw std::runtime_error("a block word differs from the rc6 set");
    }
  }
  require(!write_parameters(ram.data(), static_cast<std::uint32_t>(ram.size()), rc6_parameters()),
          "a second write changes nothing");
  for (std::uint32_t index = 0; index < 2; ++index) {
    std::uint32_t word = 0;
    std::memcpy(&word, ram.data() + 0x7F00u + index * 4, sizeof word);
    require(word == 0x11223344u + index, "write_parameters must not touch the HUD block");
  }
  std::uint32_t after = 0;
  std::memcpy(&after, ram.data() + kParameterBlock + kParameterWords * 4, sizeof after);
  require(after == above, "write_parameters must not write past the block");
  // A short buffer and a null buffer are refused without writing.
  require(!write_parameters(ram.data(), kParameterBlock, rc6_parameters()), "a short buffer is refused");
  require(!write_parameters(nullptr, static_cast<std::uint32_t>(ram.size()), rc6_parameters()), "null is refused");
  require(write_parameters(ram.data(), static_cast<std::uint32_t>(ram.size()), Parameters{}), "back to zeros");
}

void check_vblank_switch() {
  // The switch is read once (static), so this runs with GT4_FLARE_LIMITS=1 set before the first call.
  std::vector<std::uint8_t> ram(32u << 20, 0);
  at_guest_vblank(ram.data(), static_cast<std::uint32_t>(ram.size()));
  std::uint32_t flags = 0;
  std::memcpy(&flags, ram.data() + kParameterBlock, sizeof flags);
  require(flags == 7, "at_guest_vblank with GT4_FLARE_LIMITS=1 writes the limits");
  std::memset(ram.data() + kParameterBlock, 0, kParameterWords * 4);
  at_guest_vblank(ram.data(), static_cast<std::uint32_t>(ram.size()));
  std::memcpy(&flags, ram.data() + kParameterBlock, sizeof flags);
  require(flags == 7, "at_guest_vblank re-asserts the block");
}

}  // namespace

int main() {
  try {
    _putenv_s("GT4_FLARE_LIMITS", "1");
    check_rc6_words();
    check_switch();
    check_write();
    check_vblank_switch();
    std::puts("native_flare_limits: ok (rc6 words, switch, write, VBlank re-assert)");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native_flare_limits: %s\n", error.what());
    return 1;
  }
}
