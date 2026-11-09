// TASK-212: the native Quit Game row watcher on a synthetic guest RAM image.
//
// The fixture rebuilds the two objects the watcher recognises (the arcade TopRoot SelectBox with six copied rows and
// the hidden composite with the eight patched templates) and checks the append, its idempotence, the Cross capture and
// the headless halt. It never runs the game and never reads a disc.
#include "menu_quit.hpp"

#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kRam = 0x2000000u;
constexpr std::uint32_t kSelectBox = 0x00200000u;
constexpr std::uint32_t kHidden = 0x00240000u;
constexpr std::uint32_t kRows = 0x00280000u;
constexpr std::uint32_t kOwner = 0x00400000u;
constexpr std::uint32_t kSelectBoxVtable = 0x00671898u;
constexpr std::uint32_t kCompositeVtable = 0x00662EF8u;
constexpr std::uint32_t kFaceVtable = 0x00666370u;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "menu_quit fixture failed: %s\n", message);
    std::exit(1);
  }
}

void w32(std::uint8_t *ram, std::uint32_t at, std::uint32_t value) {
  std::memcpy(ram + at, &value, 4);
}

void wf32(std::uint8_t *ram, std::uint32_t at, float value) {
  std::memcpy(ram + at, &value, 4);
}

std::uint32_t u32(const std::uint8_t *ram, std::uint32_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, ram + at, 4);
  return value;
}

// One row: a composite with a label face, the geometry the SelectBox gives its copies.
void row(std::uint8_t *ram, std::uint32_t at, std::uint32_t parent, float x, float y, std::uint32_t next) {
  w32(ram, at + 4, kCompositeVtable);
  w32(ram, at + 0x14, parent);
  w32(ram, at + 0x5C, next);
  wf32(ram, at + 0x34, x);
  wf32(ram, at + 0x38, y);
  wf32(ram, at + 0x3C, 320.0f);
  wf32(ram, at + 0x40, 24.0f);
  w32(ram, at + 0xA0, 1);
  w32(ram, at + 0xA4, at + 0x200);
  w32(ram, at + 0x200 + 4, kFaceVtable);
  w32(ram, at + 0x200 + 0x14, at);
}

std::vector<std::uint8_t> build() {
  std::vector<std::uint8_t> ram(kRam, 0);
  std::uint8_t *data = ram.data();
  w32(data, 0x00622F4C, kOwner);
  w32(data, kOwner + 0x3A3BC, 0x61637261u);  // "arca"

  // Six copied rows in the SelectBox.
  for (int i = 0; i < 6; ++i) {
    const std::uint32_t at = kRows + 0x400 * static_cast<std::uint32_t>(i);
    row(data, at, kSelectBox, 64.0f, 6.0f + 24.0f * static_cast<float>(i),
        i == 5 ? 0u : kRows + 0x400 * static_cast<std::uint32_t>(i + 1));
  }
  w32(data, kSelectBox + 4, kSelectBoxVtable);
  w32(data, kSelectBox + 0xA0, 6);
  w32(data, kSelectBox + 0xA4, kRows);
  w32(data, kSelectBox + 0xA8, kRows + 0x400 * 5);
  w32(data, kSelectBox + 0xBC, 0);
  w32(data, kSelectBox + 0xC0, 0);
  w32(data, kSelectBox + 0xC4, 6);

  // Eight hidden templates; the eighth is the patched Quit Game row at the template geometry.
  for (int i = 0; i < 8; ++i) {
    const std::uint32_t at = kHidden + 0x1000 + 0x600 * static_cast<std::uint32_t>(i);
    row(data, at, kHidden, 0.0f, 0.0f,
        i == 7 ? 0u : kHidden + 0x1000 + 0x600 * static_cast<std::uint32_t>(i + 1));
  }
  w32(data, kHidden + 4, kCompositeVtable);
  w32(data, kHidden + 0xA0, 8);
  w32(data, kHidden + 0xA4, kHidden + 0x1000);
  w32(data, kHidden + 0xA8, kHidden + 0x1000 + 0x600 * 7);
  return ram;
}

} // namespace

int main() {
  gt4::menu_quit::reset();
  std::vector<std::uint8_t> ram = build();

  gt4::menu_quit::Status status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.found && status.injected, "the row is appended");
  require(status.rows == 7 && status.index == 0, "seven rows, first focused");
  require(u32(ram.data(), kSelectBox + 0xA0) == 7 && u32(ram.data(), kSelectBox + 0xA8) == kHidden + 0x1000 + 0x600 * 7,
          "the SelectBox gained the template as its last child");
  require(u32(ram.data(), kHidden + 0xA0) == 7 && u32(ram.data(), kHidden + 0xA8) == kHidden + 0x1000 + 0x600 * 6,
          "the hidden composite lost it");
  require(u32(ram.data(), kSelectBox + 0xC4) == 7, "the visible window covers seven rows");
  require(u32(ram.data(), kHidden + 0x1000 + 0x600 * 7 + 0x14) == kSelectBox, "the row is reparented");
  require(u32(ram.data(), kRows + 0x400 * 5 + 0x5C) == kHidden + 0x1000 + 0x600 * 7, "the old last row links to it");

  // Idempotent: the next VBlank does not append a second copy.
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.injected && status.rows == 7, "a second tick is a no-op");

  // Focus the last row: Cross and Circle are hidden from the guest.
  w32(ram.data(), kSelectBox + 0xBC, 6);
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.index == 6, "the last row is focused");
  require(gt4::menu_quit::guest_buttons(status, 0xFFFFu) == (0xFFFFu & ~((1u << 14) | (1u << 13))),
          "Cross and Circle are masked on the last row");
  require(gt4::menu_quit::guest_buttons(status, 0x0001u) == 0x0001u, "other buttons pass");

  // A Cross rising edge halts the headless runtime with the deliberate-quit reason.
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 1u << 14);
  require(ps2::halted(), "a Cross press on the last row halts the headless run");
  // A second press after the quit request is ignored.
  gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 1u << 14);

  std::printf("menu_quit fixture: ok\n");
  return 0;
}
