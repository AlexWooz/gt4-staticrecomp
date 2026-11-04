#include "race_display.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt4::display {
namespace {

std::uint32_t read_le32(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_le32(std::uint8_t *bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// True when the 32-bit word at `address` lies inside guest RAM.
bool word_in_ram(std::uint64_t address, std::uint32_t ram_size) {
  return address + 4 <= ram_size;
}

} // namespace

bool parse_force_480p(const char *value) {
  if (!value || std::strcmp(value, "0") == 0)
    return false;
  if (std::strcmp(value, "1") == 0)
    return true;
  throw std::invalid_argument("GT4_FORCE_480P must be 0 or 1");
}

std::uint32_t race_display_index_address(std::uint32_t owner_pointer, std::uint32_t ram_size) {
  if (!::gt4::target::available(race_display_index_offset))
    return 0;
  if (owner_pointer == 0)
    return 0;
  if (!word_in_ram(race_owner_pointer_address, ram_size))
    return 0;
  // 64-bit sums: a pointer near the top of the 32-bit range cannot wrap into RAM.
  const std::uint64_t address = static_cast<std::uint64_t>(owner_pointer) + race_option_object_offset +
                                race_display_index_offset;
  if (!word_in_ram(address, ram_size))
    return 0;
  return static_cast<std::uint32_t>(address);
}

bool should_force_480p_index(std::uint32_t index_value) {
  return index_value == race_display_index_normal || index_value == race_display_index_1080i;
}

bool force_race_480p_at_vblank(std::uint8_t *ram, std::uint32_t ram_size, Force480pState &state) {
  if (!::gt4::target::available(race_display_index_offset) ||
      !::gt4::target::available(race_owner_pointer_address)) {
    ::gt4::target::note_hook_unavailable("race display 480p (GT4_FORCE_480P)");
    return false;
  }
  if (!word_in_ram(race_owner_pointer_address, ram_size))
    return false;
  const std::uint32_t owner_pointer = read_le32(ram + race_owner_pointer_address);
  const std::uint32_t index_address = race_display_index_address(owner_pointer, ram_size);
  if (index_address == 0)
    return false;
  const std::uint32_t index_value = read_le32(ram + index_address);
  if (!should_force_480p_index(index_value))
    return false;
  write_le32(ram + index_address, race_display_index_480p);
  ++state.writes;
  if (!state.logged) {
    state.logged = true;
    std::fprintf(stderr,
                 "[display] GT4_FORCE_480P: race display index %u (%s) set to 1 (480p) at guest 0x%08X "
                 "(owner 0x%08X)\n",
                 static_cast<unsigned>(index_value), index_value == race_display_index_1080i ? "1080i" : "normal",
                 static_cast<unsigned>(index_address), static_cast<unsigned>(owner_pointer));
  }
  return true;
}

} // namespace gt4::display
