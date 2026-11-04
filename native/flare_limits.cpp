// TASK-117 (rc6): the flare-limits parameter block; see flare_limits.hpp. Pure helpers plus the VBlank write, so the
// fixture can pin every word of the block and the switch parsing.
#include "flare_limits.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gt4::flare_limits {
namespace {

std::uint32_t bits_of(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

std::uint32_t s32_bits(std::int32_t value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

}  // namespace

Parameters rc6_parameters() {
  Parameters parameters;
  parameters.words[kFlagsOffset / 4] = kAllLimits;
  parameters.words[kTrackDepthOffset / 4] = bits_of(kTrackMaxDepth);
  parameters.words[kCarDepthOffset / 4] = bits_of(kCarMaxDepth);
  parameters.words[kReservedOffset / 4] = 0;
  parameters.words[kTrackCapOffset / 4] = s32_bits(kLightCap);
  parameters.words[kCarCapOffset / 4] = s32_bits(kLightCap);
  return parameters;
}

Parameters parameters_from_switch(const char *value) {
  if (value == nullptr || *value == '\0' || std::strcmp(value, "0") == 0) return Parameters{};
  if (std::strcmp(value, "1") == 0) return rc6_parameters();
  throw std::invalid_argument(std::string(kSwitch) + " '" + value + "' is not valid (use 0 or 1)");
}

bool write_parameters(std::uint8_t *ram, std::uint32_t ram_size, const Parameters &parameters) {
  const std::uint64_t end = static_cast<std::uint64_t>(kParameterBlock) + kParameterWords * 4;
  if (ram == nullptr || end > ram_size) return false;
  bool written = false;
  for (std::uint32_t index = 0; index < kParameterWords; ++index) {
    const std::uint64_t address = static_cast<std::uint64_t>(kParameterBlock) + index * 4;
    std::uint32_t current = 0;
    std::memcpy(&current, ram + address, sizeof current);
    if (current == parameters.words[index]) continue;
    std::memcpy(ram + address, &parameters.words[index], sizeof current);
    written = true;
  }
  return written;
}

void at_guest_vblank(std::uint8_t *ram, std::uint32_t ram_size) {
  static const Parameters parameters = parameters_from_switch(std::getenv(kSwitch));
  const bool wrote = write_parameters(ram, ram_size, parameters);
  // Diagnostic (GT4_FLARE_LIMITS_LOG=1): the block must stay ours. A non-zero value there would be the game using
  // 0x7F10, and this re-assert would then be fighting it.
  if (wrote && parameters == Parameters{} && std::getenv("GT4_FLARE_LIMITS_LOG"))
    std::fprintf(stderr, "[flare-limits] guest memory at 0x7F10 was not zero; the game writes the parameter block\n");
}

}  // namespace gt4::flare_limits
