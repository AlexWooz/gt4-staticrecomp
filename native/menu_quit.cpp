// TASK-212 (slice421): append the parsed hidden::Quit widget to the main menu's SelectBox. See menu_quit.hpp.
#include "menu_quit.hpp"

#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>

namespace gt4::menu_quit {
namespace {

// v2.00 retail vtables (docs/tasks/GT4-MENU-QUIT-SETTINGS-STUDY.md): MSelectBox 0x00671898, MComposite 0x00662EF8.
constexpr std::uint32_t kSelectBoxVtable = 0x00671898u;
constexpr std::uint32_t kCompositeVtable = 0x00662EF8u;

constexpr std::uint16_t kCross = 1u << 14;
constexpr std::uint16_t kCircle = 1u << 13;

struct Tree {
  std::uint32_t selectbox = 0;
  std::uint32_t hidden = 0;
  std::uint32_t row = 0;
  bool injected = false;
};

Tree g_tree;
std::uint16_t g_previous_buttons = 0;
bool g_quit_sent = false;

std::uint32_t u32(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  if (at > size || size - at < 4) return 0;
  std::uint32_t value = 0;
  std::memcpy(&value, ram + at, 4);
  return value;
}

void w32(std::uint8_t *ram, std::size_t size, std::uint32_t at, std::uint32_t value) {
  if (at > size || size - at < 4) return;
  std::memcpy(ram + at, &value, 4);
}

void wf32(std::uint8_t *ram, std::size_t size, std::uint32_t at, float value) {
  if (at > size || size - at < 4) return;
  std::memcpy(ram + at, &value, 4);
}

bool object(const std::uint8_t *ram, std::size_t size, std::uint32_t at, std::uint32_t vtable) {
  return at >= 0x100000 && at + 0x100 <= size && u32(ram, size, at + 4) == vtable;
}

std::uint32_t first_child(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  return u32(ram, size, at + 0xA4);
}

std::uint32_t next_sibling(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  return u32(ram, size, at + 0x5C);
}

// A row is a 320x24 composite with one label child, exactly like the hidden templates.
bool row_shaped(const std::uint8_t *ram, std::size_t size, std::uint32_t at, float x, float y) {
  if (!object(ram, size, at, kCompositeVtable) || u32(ram, size, at + 0xA0) != 1) return false;
  float geometry[4];
  std::memcpy(geometry, ram + at + 0x34, sizeof geometry);
  return geometry[0] == x && geometry[1] == y && geometry[2] == 320.0f && geometry[3] == 24.0f;
}

// The main menu: a SelectBox whose visible window covers all children and whose rows are composites. The arcade
// OptionPopup has a second, two-row SelectBox; both are safe to skip.
bool find_selectbox(const std::uint8_t *ram, std::size_t size, std::uint32_t &found) {
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != kSelectBoxVtable) continue;
    const std::uint32_t base = at - 4;
    const std::uint32_t rows = u32(ram, size, base + 0xA0);
    const std::uint32_t index = u32(ram, size, base + 0xBC);
    const std::uint32_t end = u32(ram, size, base + 0xC4);
    const std::uint32_t first = first_child(ram, size, base);
    const std::uint32_t last = u32(ram, size, base + 0xA8);
    if (rows < 5 || rows > 12 || rows != end || index >= rows) continue;
    if (!row_shaped(ram, size, first, 64.0f, 6.0f)) continue;
    if (!row_shaped(ram, size, last, 64.0f, 6.0f + 24.0f * static_cast<float>(rows - 1))) continue;
    if (next_sibling(ram, size, last) != 0) continue;
    found = base;
    return true;
  }
  return false;
}

// The hidden composite after the data patch: eight row-shaped children, the last one still at the template geometry.
bool find_hidden(const std::uint8_t *ram, std::size_t size, std::uint32_t &found, std::uint32_t &row) {
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != kCompositeVtable) continue;
    const std::uint32_t base = at - 4;
    if (u32(ram, size, base + 0xA0) != 8) continue;
    std::uint32_t child = first_child(ram, size, base);
    std::uint32_t eighth = 0;
    bool shaped = true;
    for (int i = 0; i < 8 && shaped; ++i) {
      if (!row_shaped(ram, size, child, 0.0f, 0.0f) || u32(ram, size, child + 0x14) != base) {
        shaped = false;
        break;
      }
      if (i == 7) eighth = child;
      child = next_sibling(ram, size, child);
    }
    if (!shaped || eighth == 0) continue;
    found = base;
    row = eighth;
    return true;
  }
  return false;
}

Status status_from(const std::uint8_t *ram, std::size_t size) {
  Status status;
  if (!object(ram, size, g_tree.selectbox, kSelectBoxVtable)) return status;
  status.found = true;
  status.rows = static_cast<int>(u32(ram, size, g_tree.selectbox + 0xA0));
  status.index = static_cast<int>(u32(ram, size, g_tree.selectbox + 0xBC));
  status.injected = g_tree.injected && status.rows >= 1 && u32(ram, size, g_tree.selectbox + 0xA8) == g_tree.row;
  return status;
}

void inject(std::uint8_t *ram, std::size_t size) {
  const std::uint32_t box = g_tree.selectbox;
  const std::uint32_t hidden = g_tree.hidden;
  const std::uint32_t row = g_tree.row;
  const std::uint32_t rows = u32(ram, size, box + 0xA0);
  const std::uint32_t last = u32(ram, size, box + 0xA8);
  // Detach the template from hidden: its 7th child becomes the last one.
  std::uint32_t previous = first_child(ram, size, hidden);
  for (std::uint32_t i = 0; i + 1 < 7; ++i) previous = next_sibling(ram, size, previous);
  w32(ram, size, hidden + 0xA0, 7);
  w32(ram, size, hidden + 0xA8, previous);
  w32(ram, size, previous + 0x5C, 0);
  // Append it after the last menu row and extend the SelectBox window.
  w32(ram, size, row + 0x14, box);
  w32(ram, size, row + 0x5C, 0);
  wf32(ram, size, row + 0x34, 64.0f);
  wf32(ram, size, row + 0x38, 6.0f + 24.0f * static_cast<float>(rows));
  w32(ram, size, last + 0x5C, row);
  w32(ram, size, box + 0xA0, rows + 1);
  w32(ram, size, box + 0xA8, row);
  w32(ram, size, box + 0xC4, rows + 1);
}

} // namespace

Status tick(HWND game_window, std::uint8_t *ram, std::size_t size, std::uint16_t buttons) {
  Status status;
#if defined(GT4_TARGET_SPEC2)
  // Spec II's widget vtables and menu-state offsets are not located yet (TASK-212 v2.00 scope).
  (void)game_window;
  (void)size;
  static bool noted = false;
  if (!noted) {
    noted = true;
    std::fprintf(stderr, "[menu-quit] Spec II: the native row is not located yet; disabled\n");
  }
  return status;
#else
  if (!ram || size < 0x2000000) return status;
  const std::uint32_t owner = u32(ram, size, 0x00622F4C);
  // The menu state publishes the next project's four-character id; only the arcade project owns this SelectBox.
  const bool arcade = owner != 0 && u32(ram, size, owner + 0x3A3BC) == 0x61637261u;  // "arca"
  const bool cached = object(ram, size, g_tree.selectbox, kSelectBoxVtable);
  if (!cached || u32(ram, size, g_tree.selectbox + 0xA0) < 5) {
    if (!arcade) {
      g_previous_buttons = buttons;
      return status;
    }
    std::uint32_t box = 0, hidden = 0, row = 0;
    if (!find_selectbox(ram, size, box) || !find_hidden(ram, size, hidden, row)) {
      g_previous_buttons = buttons;
      return status;
    }
    g_tree.selectbox = box;
    g_tree.hidden = hidden;
    g_tree.row = row;
    g_tree.injected = false;
  }
  status = status_from(ram, size);
  if (!status.injected) {
    // Re-read the hidden count: a reloaded tree brings the template back.
    if (u32(ram, size, g_tree.hidden + 0xA0) == 8) {
      inject(ram, size);
      g_tree.injected = true;
      status = status_from(ram, size);
      std::fprintf(stderr, "[menu-quit] Quit Game row appended: %d rows, last index %d\n", status.rows, status.rows - 1);
    }
  }
  if (status.injected && status.rows > 0 && status.index == status.rows - 1) {
    const bool pressed = (buttons & kCross) != 0 && (g_previous_buttons & kCross) == 0;
    if (pressed && !g_quit_sent) {
      g_quit_sent = true;
      std::fprintf(stderr, "[menu-quit] Quit Game selected; closing the game\n");
      if (game_window) {
        PostMessageW(game_window, WM_CLOSE, 0, 0);
      } else {
        // Headless runs have no window: the halt reason is classified as a normal quit by the launcher.
        ps2::halt("quit game");
      }
    }
  }
  g_previous_buttons = buttons;
  return status;
#endif
}

std::uint16_t guest_buttons(const Status &status, std::uint16_t buttons) {
  if (status.injected && status.rows > 0 && status.index == status.rows - 1) return buttons & ~(kCross | kCircle);
  return buttons;
}

std::uint16_t scheduled_buttons() {
  const std::uint64_t frame = ps2::frame_count();
  std::uint16_t mask = 0;
  for (const ps2::Press &press : ps2::g_options.presses) {
    if (frame >= press.frame && frame < press.frame + press.frames) mask |= press.buttons;
  }
  return mask;
}

void reset() {
  g_tree = {};
  g_previous_buttons = 0;
  g_quit_sent = false;
}

} // namespace gt4::menu_quit
