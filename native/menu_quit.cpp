// TASK-212/TASK-212b: append the parsed hidden::Quit widget to the main menu's SelectBox. See menu_quit.hpp.
//
// Two targets share one algorithm; every structural difference is a field of the target profile:
//
//   v2.00 (SCUS-97328)  MSelectBox/MComposite widgets; rows are children of the SelectBox; the hidden composite
//                       holds eight templates. The arcade project is gated by the menu-state word "arca".
//   Spec II (SCUS-97436) the same engine classes, but every widget is preceded by a list-node header: the SelectBox
//                       keeps node bases and the row widget starts 0x28 bytes into its node. The hidden
//                       composite has eleven templates (ten plus the patched Quit). No reliable menu-state word was
//                       found; the SelectBox is identified by its own vtable, geometry and its StartScroll parent.
//
// Evidence for the Spec II values is in docs/tasks/QUIT-GAME-ROW-RETAIL-SPEC2.md: the headless state at VBlank 8,600
// of the arcade TopRoot, the SelectBox at 0xA95E68 (vtable 0x006D7118), rows at 0x1856700..0x18585E0 (composite
// 0x006C3EA0, node 0x006C3AD8), hidden at 0xEA83F0, Quit node 0xEA9DF0 (widget 0xEA9E18).
#include "menu_quit.hpp"

#include "game_target.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4::menu_quit {
namespace {

struct Profile {
  const char *name;
  std::uint32_t selectbox_vtable;
  std::uint32_t composite_vtable;
  std::uint32_t node_vtable;       // 0: a child pointer is the widget itself
  std::uint32_t selectbox_geometry;  // offset of the four floats
  std::uint32_t row_geometry;
  std::uint32_t hidden_geometry;
  std::uint32_t selectbox_rows;
  std::uint32_t selectbox_first;
  std::uint32_t selectbox_last;
  std::uint32_t selectbox_index;
  std::uint32_t selectbox_end;
  std::uint32_t row_next;
  std::uint32_t row_prev;          // 0: no previous link
  std::uint32_t row_node_offset;   // 0: child pointer is the widget; else widget = node + this
  std::uint32_t selectbox_node_offset;  // SelectBox widget - node base
  std::uint32_t hidden_node_offset;  // hidden widget - node base (0: the hidden object is the widget)
  std::uint32_t hidden_count;      // hidden composite child count (widget or node)
  std::uint32_t hidden_count_extra;  // second count word, 0 when the target has only one
  int hidden_children;
  std::uint32_t startscroll_node_offset;  // 0: the parent pointer is the ScrollBox widget
  std::uint32_t startscroll_geometry;
  int scan_interval;               // VBlanks between scans while the menu is absent
  bool menu_state_gate;            // v2.00 "arca" word
};

constexpr Profile kV200Profile{
    "v2.00",
    0x00671898u,  // MSelectBox vtable
    0x00662EF8u,  // MComposite vtable
    0u,           // no list-node header
    0x34u,        // SelectBox geometry
    0x34u,        // row geometry
    0x34u,        // hidden geometry
    0xA0u,        // rows
    0xA4u,        // first child
    0xA8u,        // last child
    0xBCu,        // index
    0xC4u,        // visible end
    0x5Cu,        // next sibling
    0u,           // no previous link
    0u,           // child pointer is the widget
    0u,           // SelectBox widget is the node
    0u,           // hidden widget is the node
    0xA0u,        // hidden child count (widget)
    0u,
    8,
    0u,           // parent pointer is the ScrollBox widget
    0u,           // not checked; the "arca" gate is used
    0,            // scan every VBlank while the arcade project is up
    true,
};

constexpr Profile kSpec2Profile{
    "Spec II",
    0x006D7118u,  // MSelectBox vtable
    0x006C3EA0u,  // MComposite vtable
    0x006C3AD8u,  // list-node vtable
    0x24u,        // SelectBox geometry
    0x24u,        // row geometry
    0x24u,        // hidden composite geometry
    0x90u,        // rows
    0x94u,        // first child (node)
    0x98u,        // last child (node)
    0xB8u,        // index
    0xC0u,        // visible end
    0x1Cu,        // next sibling
    0x18u,        // previous sibling
    0x28u,        // widget = node + 0x28
    0x28u,        // SelectBox widget - node
    0x28u,        // hidden widget - node
    0x20u,        // hidden child count (node)
    0x28u,        // hidden count + 1
    11,
    0x28u,        // parent node + 0x28 = ScrollBox widget
    0x24u,        // StartScroll geometry
    15,           // the structural gate has no cheap state word; scan four times a second
    false,
};

constexpr std::uint16_t kCross = 1u << 14;
constexpr std::uint16_t kCircle = 1u << 13;

struct Tree {
  std::uint32_t selectbox = 0;
  std::uint32_t hidden = 0;
  std::uint32_t row = 0;  // the value the SelectBox child list stores (widget, or node base)
  bool injected = false;
};

Tree g_tree;
std::uint16_t g_previous_buttons = 0;
int g_scan_wait = 0;
bool g_quit_sent = false;

const Profile &profile() {
  return gt4::target::active().id == gt4::target::Id::spec2 ? kSpec2Profile : kV200Profile;
}

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

float f32(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  if (at > size || size - at < 4) return 0.0f;
  float value = 0.0f;
  std::memcpy(&value, ram + at, 4);
  return value;
}

bool geometry(const std::uint8_t *ram, std::size_t size, std::uint32_t at, float x, float y, float width,
              float height) {
  float value[4];
  if (at > size || size - at < sizeof value) return false;
  std::memcpy(value, ram + at, sizeof value);
  return value[0] == x && value[1] == y && value[2] == width && value[3] == height;
}

bool object(const std::uint8_t *ram, std::size_t size, std::uint32_t at, std::uint32_t vtable) {
  return at >= 0x100000 && at + 0x100 <= size && u32(ram, size, at + 4) == vtable;
}

// The child list stores the widget (v2.00) or a list node whose widget follows a header (Spec II).
std::uint32_t row_widget(const Profile &p, std::uint32_t child) {
  if (p.row_node_offset == 0) return child;
  return child + p.row_node_offset;
}

std::uint32_t row_child(const Profile &p, std::uint32_t widget) {
  if (p.row_node_offset == 0) return widget;
  return widget - p.row_node_offset;
}

bool row_shaped(const Profile &p, const std::uint8_t *ram, std::size_t size, std::uint32_t widget, float x, float y,
                std::uint32_t parent) {
  if (!object(ram, size, widget, p.composite_vtable)) return false;
  if (!geometry(ram, size, widget + p.row_geometry, x, y, 320.0f, 24.0f)) return false;
  return parent == 0 || u32(ram, size, widget + 0x14) == parent;
}

// The main menu: a SelectBox whose visible window covers all children and whose rows are the 320x24 composites. The
// arcade OptionPopup has a second, two-row SelectBox; the vtable, geometry and StartScroll parent skip it.
bool find_selectbox(const Profile &p, const std::uint8_t *ram, std::size_t size, std::uint32_t &found) {
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != p.selectbox_vtable) continue;
    const std::uint32_t base = at - 4;
    if (!geometry(ram, size, base + p.selectbox_geometry, -64.0f, 16.0f, 460.0f, 36.0f)) continue;
    const std::uint32_t rows = u32(ram, size, base + p.selectbox_rows);
    const std::uint32_t index = u32(ram, size, base + p.selectbox_index);
    const std::uint32_t end = u32(ram, size, base + p.selectbox_end);
    if (rows < 5 || rows > 12 || rows != end || index >= rows) continue;
    const std::uint32_t node = base - p.selectbox_node_offset;
    const std::uint32_t first = u32(ram, size, base + p.selectbox_first);
    const std::uint32_t last = u32(ram, size, base + p.selectbox_last);
    const std::uint32_t first_widget = row_widget(p, first);
    // The list scrolls; the rows keep their 24 px spacing but the first row's y moves with the scroll offset.
    const float first_y = f32(ram, size, first_widget + p.row_geometry + 4);
    if (!row_shaped(p, ram, size, first_widget, 64.0f, first_y, node)) continue;
    if (!row_shaped(p, ram, size, row_widget(p, last), 64.0f,
                     first_y + 24.0f * static_cast<float>(rows - 1), node))
      continue;
    std::uint32_t child = first;
    bool shaped = true;
    for (std::uint32_t i = 0; i < rows && shaped; ++i) {
      const std::uint32_t widget = row_widget(p, child);
      if (!row_shaped(p, ram, size, widget, 64.0f, first_y + 24.0f * static_cast<float>(i), node)) {
        shaped = false;
        break;
      }
      if (i + 1 < rows) {
        child = u32(ram, size, widget + p.row_next);
      } else if (child != last || u32(ram, size, widget + p.row_next) != 0) {
        shaped = false;
      }
    }
    if (!shaped) continue;
    if (p.startscroll_geometry != 0) {
      const std::uint32_t parent = u32(ram, size, base + 0x14);
      const std::uint32_t scroll =
          p.startscroll_node_offset == 0 ? parent : parent + p.startscroll_node_offset;
      if (!geometry(ram, size, scroll + p.startscroll_geometry, 160.0f, 332.0f, 320.0f, 68.0f)) continue;
    }
    found = base;
    return true;
  }
  return false;
}

// The hidden composite with the patched template, and the template's child value (the last one).
bool find_hidden(const Profile &p, const std::uint8_t *ram, std::size_t size, std::uint32_t &found,
                 std::uint32_t &row) {
  if (p.row_node_offset == 0) {
    for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
      if (u32(ram, size, at) != p.composite_vtable) continue;
      const std::uint32_t base = at - 4;
      if (u32(ram, size, base + p.hidden_count) != static_cast<std::uint32_t>(p.hidden_children)) continue;
      std::uint32_t child = u32(ram, size, base + p.selectbox_first);
      std::uint32_t last = 0;
      bool shaped = true;
      for (int i = 0; i < p.hidden_children && shaped; ++i) {
        if (!row_shaped(p, ram, size, child, 0.0f, 0.0f, base)) {
          shaped = false;
          break;
        }
        if (i + 1 == p.hidden_children) last = child;
        child = u32(ram, size, child + p.row_next);
      }
      if (!shaped || last == 0) continue;
      found = base;
      row = last;
      return true;
    }
    return false;
  }
  // Spec II: scan the composite widgets with the template geometry, then follow the list nodes that point at the
  // hidden node. The last sibling is the patched Quit row.
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != p.composite_vtable) continue;
    const std::uint32_t widget = at - 4;
    if (!geometry(ram, size, widget + p.hidden_geometry, 0.0f, 0.0f, 320.0f, 24.0f)) continue;
    const std::uint32_t node = widget - p.hidden_node_offset;
    if (!object(ram, size, node, p.node_vtable)) continue;
    if (u32(ram, size, node + p.hidden_count) != static_cast<std::uint32_t>(p.hidden_children)) continue;
    std::uint32_t first = 0;
    for (std::uint32_t child = 0x100000; child + 0x100 <= size; child += 4) {
      if (u32(ram, size, child + 4) != p.node_vtable) continue;
      const std::uint32_t child_node = child;
      if (u32(ram, size, child_node + 0x14) != node) continue;
      const std::uint32_t child_widget = child_node + p.row_node_offset;
      if (!row_shaped(p, ram, size, child_widget, 0.0f, 0.0f, node)) continue;
      if (u32(ram, size, child_widget + p.row_prev) == 0) {
        first = child_node;
        break;
      }
    }
    if (first == 0) continue;
    std::uint32_t child_node = first;
    bool shaped = true;
    for (int i = 0; i < p.hidden_children && shaped; ++i) {
      const std::uint32_t child_widget = child_node + p.row_node_offset;
      if (!row_shaped(p, ram, size, child_widget, 0.0f, 0.0f, node)) {
        shaped = false;
        break;
      }
      if (i + 1 == p.hidden_children) {
        found = node;
        row = row_child(p, child_widget);
        return true;
      }
      const std::uint32_t next = u32(ram, size, child_widget + p.row_next);
      if (next == 0) {
        shaped = false;
        break;
      }
      child_node = next;
    }
  }
  return false;
}

Status status_from(const Profile &p, const std::uint8_t *ram, std::size_t size) {
  Status status;
  if (!object(ram, size, g_tree.selectbox, p.selectbox_vtable)) return status;
  status.found = true;
  status.rows = static_cast<int>(u32(ram, size, g_tree.selectbox + p.selectbox_rows));
  status.index = static_cast<int>(u32(ram, size, g_tree.selectbox + p.selectbox_index));
  status.injected =
      g_tree.injected && status.rows >= 1 && u32(ram, size, g_tree.selectbox + p.selectbox_last) == g_tree.row;
  return status;
}

void detach_template(const Profile &p, std::uint8_t *ram, std::size_t size) {
  const std::uint32_t hidden = g_tree.hidden;
  const std::uint32_t row = g_tree.row;
  const std::uint32_t widget = row_widget(p, row);
  if (p.row_node_offset == 0) {
    // v2.00: the hidden composite is the template list itself. Its previous child becomes the last one.
    std::uint32_t previous = u32(ram, size, hidden + p.selectbox_first);
    for (int i = 0; i + 1 < p.hidden_children - 1; ++i) previous = u32(ram, size, previous + p.row_next);
    w32(ram, size, hidden + p.hidden_count, static_cast<std::uint32_t>(p.hidden_children - 1));
    w32(ram, size, hidden + p.selectbox_last, previous);
    w32(ram, size, previous + p.row_next, 0);
    return;
  }
  // Spec II: clear the previous sibling (10th template) and the hidden node's count pair.
  const std::uint32_t previous = u32(ram, size, widget + p.row_prev);
  if (previous != 0) w32(ram, size, row_widget(p, previous) + p.row_next, 0);
  w32(ram, size, widget + p.row_prev, 0);
  w32(ram, size, hidden + p.hidden_count, static_cast<std::uint32_t>(p.hidden_children - 1));
  if (p.hidden_count_extra != 0)
    w32(ram, size, hidden + p.hidden_count_extra, static_cast<std::uint32_t>(p.hidden_children));
}

void inject(const Profile &p, std::uint8_t *ram, std::size_t size) {
  const std::uint32_t box = g_tree.selectbox;
  const std::uint32_t row = g_tree.row;
  const std::uint32_t widget = row_widget(p, row);
  const std::uint32_t node = box - p.selectbox_node_offset;
  const std::uint32_t rows = u32(ram, size, box + p.selectbox_rows);
  const std::uint32_t last = u32(ram, size, box + p.selectbox_last);
  const std::uint32_t last_widget = row_widget(p, last);
  detach_template(p, ram, size);
  // Link it after the last menu row and extend the SelectBox window.
  if (p.row_prev != 0) w32(ram, size, widget + p.row_prev, last);
  w32(ram, size, widget + p.row_next, 0);
  w32(ram, size, widget + 0x14, node);
  if (p.row_node_offset != 0) w32(ram, size, widget - p.row_node_offset + 0x14, node);
  wf32(ram, size, widget + p.row_geometry, 64.0f);
  wf32(ram, size, widget + p.row_geometry + 4, 6.0f + 24.0f * static_cast<float>(rows));
  w32(ram, size, last_widget + p.row_next, row);
  w32(ram, size, box + p.selectbox_last, row);
  w32(ram, size, box + p.selectbox_rows, rows + 1);
  w32(ram, size, box + p.selectbox_end, rows + 1);
}

} // namespace

Status tick(HWND game_window, std::uint8_t *ram, std::size_t size, std::uint16_t buttons) {
  Status status;
  if (!ram || size < 0x2000000) return status;
  const Profile &p = profile();
  static bool announced = false;
  if (!announced) {
    announced = true;
    std::fprintf(stderr, "[menu-quit] %s watcher active\n", p.name);
  }

  if (p.menu_state_gate) {
    const std::uint32_t owner = u32(ram, size, 0x00622F4C);
    // The menu state publishes the next project's four-character id; only the arcade project owns this SelectBox.
    const bool arcade = owner != 0 && u32(ram, size, owner + 0x3A3BC) == 0x61637261u;  // "arca"
    if (!arcade && !object(ram, size, g_tree.selectbox, p.selectbox_vtable)) {
      g_previous_buttons = buttons;
      return status;
    }
  }

  const bool cached = object(ram, size, g_tree.selectbox, p.selectbox_vtable) &&
                      u32(ram, size, g_tree.selectbox + p.selectbox_rows) >= 5;
  if (!cached) {
    if (g_scan_wait > 0) {
      --g_scan_wait;
      g_previous_buttons = buttons;
      return status;
    }
    g_scan_wait = p.scan_interval;
    std::uint32_t box = 0, hidden = 0, row = 0;
    const bool found_box = find_selectbox(p, ram, size, box);
    const bool found_hidden = found_box && find_hidden(p, ram, size, hidden, row);
    if (std::getenv("GT4_MENU_QUIT_TRACE") != nullptr)
      std::fprintf(stderr, "[menu-quit] scan %llu: box %d hidden %d\n",
                   static_cast<unsigned long long>(ps2::frame_count()), found_box ? 1 : 0, found_hidden ? 1 : 0);
    if (!found_box || !found_hidden) {
      g_previous_buttons = buttons;
      return status;
    }
    g_tree.selectbox = box;
    g_tree.hidden = hidden;
    g_tree.row = row;
    g_tree.injected = false;
    std::fprintf(stderr, "[menu-quit] %s: SelectBox 0x%08X, hidden 0x%08X, template 0x%08X\n", p.name, box, hidden,
                 row);
  }
  status = status_from(p, ram, size);
  if (!status.injected) {
    // A reloaded tree brings the template back; the hidden count is the signal.
    if (u32(ram, size, g_tree.hidden + p.hidden_count) == static_cast<std::uint32_t>(p.hidden_children)) {
      inject(p, ram, size);
      g_tree.injected = true;
      status = status_from(p, ram, size);
      std::fprintf(stderr, "[menu-quit] Quit Game row appended: %d rows, last index %d\n", status.rows,
                   status.rows - 1);
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
  g_scan_wait = 0;
  g_quit_sent = false;
}

} // namespace gt4::menu_quit
