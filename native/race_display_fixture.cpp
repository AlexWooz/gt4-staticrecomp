// Fixture for the slice164 race display index. Guest RAM is a synthetic 32 MB array with the owner
// pointer at 0x622F4C; the offsets are the pinned ELF chain (0x622F4C -> +0x38CB0 -> +0x16B0).

#include "race_display.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using namespace gt4::display;

constexpr std::uint32_t ram_size = 32u << 20;
constexpr std::uint32_t owner = 0x00300000u;
constexpr std::uint32_t index_address = owner + 0x38CB0u + 0x16B0u;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void store(std::vector<std::uint8_t> &ram, std::uint32_t address, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    ram[address + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t load(const std::vector<std::uint8_t> &ram, std::uint32_t address) {
  return static_cast<std::uint32_t>(ram[address]) | (static_cast<std::uint32_t>(ram[address + 1]) << 8) |
         (static_cast<std::uint32_t>(ram[address + 2]) << 16) |
         (static_cast<std::uint32_t>(ram[address + 3]) << 24);
}

std::vector<std::uint8_t> make_ram(std::uint32_t owner_pointer, std::uint32_t index_value) {
  std::vector<std::uint8_t> ram(ram_size, 0);
  store(ram, race_owner_pointer_address, owner_pointer);
  if (owner_pointer != 0 && index_address + 4 <= ram_size)
    store(ram, index_address, index_value);
  return ram;
}

void test_environment_parsing() {
  require(!parse_force_480p(nullptr), "unset means off");
  require(!parse_force_480p("0"), "0 means off");
  require(parse_force_480p("1"), "1 means on");
  bool refused = false;
  try {
    parse_force_480p("480p");
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "unknown values are refused");
}

void test_address_chain() {
  require(race_display_index_address(owner, ram_size) == index_address, "owner pointer chain");
  require(race_display_index_address(0, ram_size) == 0, "null owner is not a race object");
  require(race_display_index_address(ram_size - 0x3A360u + 1, ram_size) == 0, "index outside RAM is refused");
  require(race_display_index_address(0xFFFFFFF0u, ram_size) == 0, "wrapping owner is refused");
  require(race_display_index_address(ram_size - 0x3A360u - 4, ram_size) ==
              ram_size - 0x3A360u - 4 + 0x3A360u,
          "last valid index word is accepted");
}

void test_write_decision() {
  require(should_force_480p_index(race_display_index_normal), "normal index is forced");
  require(!should_force_480p_index(race_display_index_480p), "480p index is kept");
  require(should_force_480p_index(race_display_index_1080i), "1080i index is forced (TASK-078)");
  require(!should_force_480p_index(3u), "an unknown index is left alone");
}

void test_vblank_write() {
  Force480pState state;
  auto ram = make_ram(owner, race_display_index_normal);
  require(force_race_480p_at_vblank(ram.data(), ram_size, state), "normal index is written at VBlank");
  require(load(ram, index_address) == race_display_index_480p, "index becomes 480p");
  require(state.writes == 1 && state.logged, "one write is counted and logged");
  require(force_race_480p_at_vblank(ram.data(), ram_size, state) == false, "480p index is not rewritten");
  require(state.writes == 1, "no second write for 480p");
}

// TASK-078: a save or an imported card that holds 1080i races at 480p too.
void test_1080i_is_held_at_480p() {
  Force480pState state;
  auto ram = make_ram(owner, race_display_index_1080i);
  require(force_race_480p_at_vblank(ram.data(), ram_size, state), "1080i index is written at VBlank");
  require(load(ram, index_address) == race_display_index_480p, "1080i value becomes 480p");
  require(state.writes == 1, "one write counted for 1080i");
}

void test_invalid_owner_is_ignored() {
  Force480pState state;
  auto ram = make_ram(0, race_display_index_normal);
  require(!force_race_480p_at_vblank(ram.data(), ram_size, state), "null owner writes nothing");
  require(state.writes == 0, "null owner is not counted");
  auto far = make_ram(0xFFFFFFF0u, race_display_index_normal);
  require(!force_race_480p_at_vblank(far.data(), ram_size, state), "out-of-range owner writes nothing");
}

} // namespace

int main() {
  test_environment_parsing();
  test_address_chain();
  test_write_decision();
  test_vblank_write();
  test_1080i_is_held_at_480p();
  test_invalid_owner_is_ignored();
  std::puts("race_display fixture OK");
  return 0;
}
