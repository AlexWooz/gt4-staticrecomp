// TASK-212b: the native Quit Game row watcher on a synthetic Spec II guest RAM image.
//
// The Spec II engine wraps every widget in a 0x28-byte list node: the SelectBox stores node pointers and the row widget
// starts 0x28 bytes into the node. This fixture rebuilds that layout (the values are the ones found in the headless
// arcade TopRoot state; see docs/tasks/QUIT-GAME-ROW-RETAIL-SPEC2.md) and checks the append, the template detach, the
// Cross capture and the headless halt. It never runs the game and never reads a disc.
#include "menu_quit.hpp"

#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kRam = 0x2000000u;
constexpr std::uint32_t kScrollNode = 0x001FF000u;
constexpr std::uint32_t kSelectBox = 0x00200000u;
constexpr std::uint32_t kHiddenNode = 0x00240000u;
constexpr std::uint32_t kRows = 0x00280000u;
constexpr std::uint32_t kSelectBoxVtable = 0x006D7118u;
constexpr std::uint32_t kCompositeVtable = 0x006C3EA0u;
constexpr std::uint32_t kNodeVtable = 0x006C3AD8u;
constexpr std::uint32_t kFaceVtable = 0x006C3ED0u;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "menu_quit spec2 fixture failed: %s\n", message);
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

// One Spec II widget: a 0x28-byte list node (vtable 0x6C3AD8) at `node`, the widget at node + 0x28. A row has one
// label child below the node.
void row(std::uint8_t *ram, std::uint32_t node, std::uint32_t parent, float x, float y, std::uint32_t prev,
         std::uint32_t next) {
  const std::uint32_t widget = node + 0x28;
  w32(ram, node + 4, kNodeVtable);
  w32(ram, node + 0x14, parent);
  w32(ram, node + 0x28, 8);  // widget's own prefix word
  w32(ram, widget + 4, kCompositeVtable);
  w32(ram, widget + 0x14, parent);
  w32(ram, widget + 0x18, prev);
  w32(ram, widget + 0x1C, next);
  wf32(ram, widget + 0x24, x);
  wf32(ram, widget + 0x28, y);
  wf32(ram, widget + 0x2C, 320.0f);
  wf32(ram, widget + 0x30, 24.0f);
  w32(ram, widget + 0x40, 1);
  w32(ram, widget + 0x44, node + 0x400);
  w32(ram, node + 0x400 + 4, kFaceVtable);
  w32(ram, node + 0x400 + 0x14, node);
}

std::vector<std::uint8_t> build() {
  std::vector<std::uint8_t> ram(kRam, 0);
  std::uint8_t *data = ram.data();
  // StartScroll: the SelectBox's parent pointer is the ScrollBox node; its widget is node + 0x28.
  w32(data, kScrollNode + 4, kNodeVtable);
  wf32(data, kScrollNode + 0x28 + 0x24, 160.0f);
  wf32(data, kScrollNode + 0x28 + 0x28, 332.0f);
  wf32(data, kScrollNode + 0x28 + 0x2C, 320.0f);
  wf32(data, kScrollNode + 0x28 + 0x30, 68.0f);
  const std::uint32_t selectbox_node = kSelectBox - 0x28;
  w32(data, selectbox_node + 4, kNodeVtable);
  w32(data, kSelectBox + 4, kSelectBoxVtable);
  w32(data, kSelectBox + 0x14, kScrollNode);
  wf32(data, kSelectBox + 0x24, -64.0f);
  wf32(data, kSelectBox + 0x28, 16.0f);
  wf32(data, kSelectBox + 0x2C, 460.0f);
  wf32(data, kSelectBox + 0x30, 36.0f);

  // Seven copied rows in the SelectBox; the child list stores the node base.
  for (int i = 0; i < 7; ++i) {
    const std::uint32_t node = kRows + 0x400 * static_cast<std::uint32_t>(i);
    const std::uint32_t prev = i == 0 ? 0u : kRows + 0x400 * static_cast<std::uint32_t>(i - 1);
    const std::uint32_t next = i == 6 ? 0u : kRows + 0x400 * static_cast<std::uint32_t>(i + 1);
    row(data, node, selectbox_node, 64.0f, 6.0f + 24.0f * static_cast<float>(i), prev, next);
  }
  w32(data, kSelectBox + 0x90, 7);
  w32(data, kSelectBox + 0x94, kRows);
  w32(data, kSelectBox + 0x98, kRows + 0x400 * 6);
  w32(data, kSelectBox + 0xB8, 0);
  w32(data, kSelectBox + 0xC0, 7);

  // Eleven hidden templates; the eleventh is the patched Quit Game row. The hidden composite's own widget starts at
  // node + 0x28 and its geometry lives at +0x24.
  w32(data, kHiddenNode + 4, kNodeVtable);
  w32(data, kHiddenNode + 0x20, 11);
  w32(data, kHiddenNode + 0x28, 12);
  w32(data, kHiddenNode + 0x28 + 4, kCompositeVtable);
  wf32(data, kHiddenNode + 0x28 + 0x24, 0.0f);
  wf32(data, kHiddenNode + 0x28 + 0x28, 0.0f);
  wf32(data, kHiddenNode + 0x28 + 0x2C, 320.0f);
  wf32(data, kHiddenNode + 0x28 + 0x30, 24.0f);
  for (int i = 0; i < 11; ++i) {
    const std::uint32_t node = kHiddenNode + 0x1000 + 0x600 * static_cast<std::uint32_t>(i);
    const std::uint32_t prev = i == 0 ? 0u : kHiddenNode + 0x1000 + 0x600 * static_cast<std::uint32_t>(i - 1);
    const std::uint32_t next = i == 10 ? 0u : kHiddenNode + 0x1000 + 0x600 * static_cast<std::uint32_t>(i + 1);
    row(data, node, kHiddenNode, 0.0f, 0.0f, prev, next);
  }
  return ram;
}

} // namespace

int main() {
  gt4::menu_quit::reset();
  std::vector<std::uint8_t> ram = build();

  gt4::menu_quit::Status status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.found && status.injected, "the row is appended");
  require(status.rows == 8 && status.index == 0, "eight rows, first focused");
  const std::uint32_t quit = kHiddenNode + 0x1000 + 0x600 * 10;
  const std::uint32_t last = kRows + 0x400 * 6;
  require(u32(ram.data(), kSelectBox + 0x90) == 8 && u32(ram.data(), kSelectBox + 0x98) == quit,
          "the SelectBox gained the template as its last child");
  require(u32(ram.data(), kSelectBox + 0xC0) == 8, "the visible window covers eight rows");
  require(u32(ram.data(), kHiddenNode + 0x20) == 10 && u32(ram.data(), kHiddenNode + 0x28) == 11,
          "the hidden composite lost it");
  require(u32(ram.data(), kHiddenNode + 0x1000 + 0x600 * 9 + 0x28 + 0x1C) == 0,
          "the tenth template is now the last one");
  require(u32(ram.data(), quit + 0x28 + 0x18) == last, "the template is linked after Credit");
  require(u32(ram.data(), quit + 0x14) == kSelectBox - 0x28, "the node is reparented to the SelectBox");
  require(u32(ram.data(), quit + 0x28 + 0x14) == kSelectBox - 0x28, "the widget is reparented too");
  require(u32(ram.data(), last + 0x28 + 0x1C) == quit, "the old last row links to it");
  const float y = [&] {
    float value = 0;
    std::memcpy(&value, ram.data() + quit + 0x28 + 0x28, 4);
    return value;
  }();
  require(y == 6.0f + 24.0f * 7.0f, "the appended row sits below Credit");

  // Idempotent: the next VBlank does not append a second copy.
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.injected && status.rows == 8, "a second tick is a no-op");

  // Focus the last row: Cross and Circle are hidden from the guest.
  w32(ram.data(), kSelectBox + 0xB8, 7);
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  require(status.index == 7, "the last row is focused");
  require(gt4::menu_quit::guest_buttons(status, 0xFFFFu) == (0xFFFFu & ~((1u << 14) | (1u << 13))),
          "Cross and Circle are masked on the last row");
  require(gt4::menu_quit::guest_buttons(status, 0x0001u) == 0x0001u, "other buttons pass");

  // A Cross rising edge halts the headless runtime with the deliberate-quit reason.
  status = gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 1u << 14);
  require(ps2::halted(), "a Cross press on the last row halts the headless run");
  gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 0);
  gt4::menu_quit::tick(nullptr, ram.data(), ram.size(), 1u << 14);

  std::printf("menu_quit spec2 fixture: ok\n");
  return 0;
}
