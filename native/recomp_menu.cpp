// TASK-213 Phase 2a: the Recomp settings host menu on the main menu SelectBox. See recomp_menu.hpp.
#include "recomp_menu.hpp"

#include "native_settings.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace gt4::recomp_menu {
namespace {

constexpr std::uint32_t kCompositeVtable = 0x00662EF8u;
constexpr std::uint32_t kSelectBoxVtable = 0x00671898u;
constexpr std::uint32_t kSelectBoxGeometry = 0x34u;
constexpr std::uint32_t kRows = 0xA0u;
constexpr std::uint32_t kFirst = 0xA4u;
constexpr std::uint32_t kLast = 0xA8u;
constexpr std::uint32_t kBoxIndex = 0xBCu;
constexpr std::uint32_t kEnd = 0xC4u;
constexpr std::uint32_t kNext = 0x5Cu;
constexpr std::uint32_t kParent = 0x14u;
constexpr std::uint32_t kRowGeometry = 0x34u;
constexpr std::uint32_t kState = 0x98u;
constexpr std::uint32_t kInsensitiveBit = 3u;
constexpr std::uint32_t kRecompCount = 20u;
constexpr int kEntryRow = 0;
constexpr int kHubRows[5] = {1, 2, 3, 4, 5};
constexpr int kRendererRows[2] = {6, 7};
constexpr int kScaleRows[6] = {8, 9, 10, 11, 12, 13};
constexpr int kHubTitle = 14;
constexpr int kGraphicsTitle = 15;
constexpr int kControlsTitle = 16;
constexpr int kDisplayTitle = 17;
constexpr int kAudioTitle = 18;
constexpr int kSystemTitle = 19;
constexpr std::uint16_t kCross = 1u << 14;
constexpr std::uint16_t kCircle = 1u << 13;
constexpr std::uint16_t kUp = 1u << 4;
constexpr std::uint16_t kDown = 1u << 6;

enum class Mode { main, hub, graphics, shell };

struct BoxFields {
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  std::uint32_t rows = 0;
  std::uint32_t index = 0;
  std::uint32_t end = 0;
};

std::uint32_t g_box = 0;
std::uint32_t g_rows[kRecompCount] = {};
bool g_ready = false;
bool g_entry = false;
int g_entry_index = -1;
Mode g_mode = Mode::main;
BoxFields g_main{};
BoxFields g_hub{};
BoxFields g_active{};
bool g_active_valid = false;
int g_shell_title = -1;
int g_renderer = 0;
int g_scale = 2;
bool g_renderer_changed = false;
bool g_scale_changed = false;
std::uint16_t g_previous_buttons = 0;
int g_current_index = -1;

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

float f32(const std::uint8_t *ram, std::size_t size, std::uint32_t at) {
  if (at > size || size - at < 4) return 0.0f;
  float value = 0.0f;
  std::memcpy(&value, ram + at, 4);
  return value;
}

void wf32(std::uint8_t *ram, std::size_t size, std::uint32_t at, float value) {
  if (at > size || size - at < 4) return;
  std::memcpy(ram + at, &value, 4);
}

bool geometry(const std::uint8_t *ram, std::size_t size, std::uint32_t at, float x, float y, float width,
              float height) {
  return f32(ram, size, at) == x && f32(ram, size, at + 4) == y && f32(ram, size, at + 8) == width &&
         f32(ram, size, at + 12) == height;
}

bool object(const std::uint8_t *ram, std::size_t size, std::uint32_t at, std::uint32_t vtable) {
  return at >= 0x100000 && at + 0x100 <= size && u32(ram, size, at + 4) == vtable;
}

bool row_shaped(const std::uint8_t *ram, std::size_t size, std::uint32_t row, std::uint32_t parent, float y) {
  return object(ram, size, row, kCompositeVtable) && geometry(ram, size, row + kRowGeometry, 64.0f, y, 320.0f, 24.0f) &&
         u32(ram, size, row + kParent) == parent;
}

bool find_selectbox(const std::uint8_t *ram, std::size_t size, std::uint32_t &box) {
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != kSelectBoxVtable) continue;
    const std::uint32_t base = at - 4;
    if (!geometry(ram, size, base + kSelectBoxGeometry, -64.0f, 16.0f, 460.0f, 36.0f)) continue;
    const std::uint32_t rows = u32(ram, size, base + kRows);
    const std::uint32_t index = u32(ram, size, base + kBoxIndex);
    const std::uint32_t end = u32(ram, size, base + kEnd);
    if (rows < 5 || rows > 12 || rows != end || index >= rows) continue;
    const std::uint32_t first = u32(ram, size, base + kFirst);
    const std::uint32_t last = u32(ram, size, base + kLast);
    const float first_y = f32(ram, size, first + kRowGeometry + 4);
    if (!row_shaped(ram, size, first, base, first_y)) continue;
    if (!row_shaped(ram, size, last, base, first_y + 24.0f * static_cast<float>(rows - 1))) continue;
    std::uint32_t child = first;
    bool shaped = true;
    for (std::uint32_t i = 0; i < rows && shaped; ++i) {
      if (!row_shaped(ram, size, child, base, first_y + 24.0f * static_cast<float>(i))) {
        shaped = false;
        break;
      }
      if (i + 1 < rows) {
        child = u32(ram, size, child + kNext);
      } else if (child != last || u32(ram, size, child + kNext) != 0) {
        shaped = false;
      }
    }
    if (!shaped) continue;
    box = base;
    return true;
  }
  return false;
}

bool find_recomp(const std::uint8_t *ram, std::size_t size, std::uint32_t &hidden) {
  for (std::uint32_t at = 0x100000; at + 0x100 <= size; at += 4) {
    if (u32(ram, size, at) != kCompositeVtable) continue;
    const std::uint32_t base = at - 4;
    if (!geometry(ram, size, base + kRowGeometry, 0.0f, 0.0f, 320.0f, 24.0f)) continue;
    if (u32(ram, size, base + kRows) != kRecompCount) continue;
    const std::uint32_t first = u32(ram, size, base + kFirst);
    const std::uint32_t last = u32(ram, size, base + kLast);
    std::uint32_t child = first;
    bool shaped = true;
    for (std::uint32_t i = 0; i < kRecompCount && shaped; ++i) {
      if (!object(ram, size, child, kCompositeVtable) ||
          !geometry(ram, size, child + kRowGeometry, 0.0f, 0.0f, 320.0f, 24.0f) ||
          u32(ram, size, child + kParent) != base) {
        shaped = false;
        break;
      }
      g_rows[i] = child;
      if (i + 1 < kRecompCount) {
        child = u32(ram, size, child + kNext);
      } else if (child != last || u32(ram, size, child + kNext) != 0) {
        shaped = false;
      }
    }
    if (!shaped) continue;
    hidden = base;
    return true;
  }
  return false;
}

void set_insensitive(std::uint8_t *ram, std::size_t size, std::uint32_t row, bool on) {
  std::uint32_t word = u32(ram, size, row + kState);
  if (on)
    word |= 1u << kInsensitiveBit;
  else
    word &= ~(1u << kInsensitiveBit);
  w32(ram, size, row + kState, word);
}

BoxFields read_fields(const std::uint8_t *ram, std::size_t size, std::uint32_t box) {
  BoxFields fields;
  fields.first = u32(ram, size, box + kFirst);
  fields.last = u32(ram, size, box + kLast);
  fields.rows = u32(ram, size, box + kRows);
  fields.index = u32(ram, size, box + kBoxIndex);
  fields.end = u32(ram, size, box + kEnd);
  return fields;
}

void write_fields(std::uint8_t *ram, std::size_t size, std::uint32_t box, const BoxFields &fields) {
  w32(ram, size, box + kFirst, fields.first);
  w32(ram, size, box + kLast, fields.last);
  w32(ram, size, box + kRows, fields.rows);
  w32(ram, size, box + kBoxIndex, fields.index);
  w32(ram, size, box + kEnd, fields.end);
}

void set_list(std::uint8_t *ram, std::size_t size, std::initializer_list<int> rows, int index) {
  std::uint32_t previous = 0;
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  int position = 0;
  for (const int which : rows) {
    if (which < 0 || which >= static_cast<int>(kRecompCount)) continue;
    const std::uint32_t row = g_rows[which];
    if (previous != 0) w32(ram, size, previous + kNext, row);
    else first = row;
    w32(ram, size, row + kParent, g_box);
    wf32(ram, size, row + kRowGeometry, 64.0f);
    wf32(ram, size, row + kRowGeometry + 4, 6.0f + 24.0f * static_cast<float>(position));
    set_insensitive(ram, size, row, which >= static_cast<int>(kHubTitle));
    previous = row;
    last = row;
    ++position;
  }
  if (last != 0) w32(ram, size, last + kNext, 0);
  BoxFields fields;
  fields.first = first;
  fields.last = last;
  fields.rows = static_cast<std::uint32_t>(position);
  fields.index = static_cast<std::uint32_t>(index);
  fields.end = static_cast<std::uint32_t>(position);
  g_active = fields;
  g_active_valid = true;
  g_current_index = index;
  write_fields(ram, size, g_box, fields);
}

bool entry_present(const std::uint8_t *ram, std::size_t size, std::uint32_t box) {
  const std::uint32_t rows = u32(ram, size, box + kRows);
  std::uint32_t row = u32(ram, size, box + kFirst);
  for (std::uint32_t i = 0; i < rows && row != 0; ++i) {
    if (row == g_rows[kEntryRow]) return true;
    row = u32(ram, size, row + kNext);
  }
  return false;
}

int find_entry_index(const std::uint8_t *ram, std::size_t size, std::uint32_t box) {
  const std::uint32_t rows = u32(ram, size, box + kRows);
  std::uint32_t row = u32(ram, size, box + kFirst);
  for (std::uint32_t i = 0; i < rows && row != 0; ++i) {
    if (row == g_rows[kEntryRow]) return static_cast<int>(i);
    row = u32(ram, size, row + kNext);
  }
  return -1;
}

int renderer_row() { return kRendererRows[g_renderer]; }
int scale_row() { return kScaleRows[g_scale]; }

std::string quote(const std::string &value) {
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '\\' || character == '"') quoted += '\\';
    quoted += character;
  }
  quoted += '"';
  return quoted;
}

void write_changes() {
  const char *keys[2] = {};
  const std::string renderer = quote(g_renderer == 0 ? "pcsx2" : "coregs20");
  char scale_text[8];
  std::snprintf(scale_text, sizeof scale_text, "%d", 50 + 25 * g_scale);
  const std::string scale = quote(scale_text);
  const char *values[2] = {};
  std::size_t count = 0;
  if (g_renderer_changed) {
    keys[count] = "renderer";
    values[count] = renderer.c_str();
    ++count;
  }
  if (g_scale_changed) {
    keys[count] = "render_scale";
    values[count] = scale.c_str();
    ++count;
  }
  if (count == 0) return;
  char error[160] = {};
  if (gt4::native_settings::write_feedback_changes(keys, values, count, error, sizeof error)) {
    for (std::size_t i = 0; i < count; ++i)
      std::fprintf(stderr, "[recomp-menu] %s = %s (restart)\n", keys[i], values[i]);
  } else {
    std::fprintf(stderr, "[recomp-menu] feedback not written: %s\n", error);
  }
}

void open_hub(std::uint8_t *ram, std::size_t size) {
  if (!g_ready || !object(ram, size, g_box, kSelectBoxVtable)) return;
  g_main = read_fields(ram, size, g_box);
  set_list(ram, size, {kHubTitle, kHubRows[0], kHubRows[1], kHubRows[2], kHubRows[3], kHubRows[4]}, 1);
  g_mode = Mode::hub;
  std::fprintf(stderr, "[recomp-menu] hub open: Graphics, Controls, Display, Audio, System\n");
}

void open_graphics(std::uint8_t *ram, std::size_t size) {
  g_hub = read_fields(ram, size, g_box);
  set_list(ram, size, {kGraphicsTitle, renderer_row(), scale_row()}, 1);
  g_mode = Mode::graphics;
  std::fprintf(stderr, "[recomp-menu] Graphics open: renderer %s, render scale %d%%\n",
               g_renderer == 0 ? "GSdx" : "CoreGS 2.0", 50 + 25 * g_scale);
}

void open_shell(std::uint8_t *ram, std::size_t size, int title) {
  g_hub = read_fields(ram, size, g_box);
  g_shell_title = title;
  set_list(ram, size, {title}, 0);
  g_mode = Mode::shell;
  std::fprintf(stderr, "[recomp-menu] shell open: row %d\n", title);
}

void back_main(std::uint8_t *ram, std::size_t size) {
  write_fields(ram, size, g_box, g_main);
  g_active = g_main;
  g_active_valid = true;
  g_mode = Mode::main;
  std::fprintf(stderr, "[recomp-menu] back to the main menu\n");
}

void back_hub(std::uint8_t *ram, std::size_t size) {
  write_fields(ram, size, g_box, g_hub);
  g_active = g_hub;
  g_active_valid = true;
  g_current_index = static_cast<int>(g_hub.index);
  g_mode = Mode::hub;
  std::fprintf(stderr, "[recomp-menu] back to the hub\n");
}

void toggle_renderer(std::uint8_t *ram, std::size_t size) {
  g_renderer ^= 1;
  set_list(ram, size, {kGraphicsTitle, renderer_row(), scale_row()}, 1);
  g_renderer_changed = true;
  write_changes();
  std::fprintf(stderr, "[recomp-menu] renderer -> %s\n", g_renderer == 0 ? "GSdx" : "CoreGS 2.0");
}

void next_scale(std::uint8_t *ram, std::size_t size) {
  g_scale = (g_scale + 1) % 6;
  set_list(ram, size, {kGraphicsTitle, renderer_row(), scale_row()}, 2);
  char text[8];
  std::snprintf(text, sizeof text, "%d", 50 + 25 * g_scale);
  g_scale_changed = true;
  write_changes();
  std::fprintf(stderr, "[recomp-menu] render scale -> %s%%\n", text);
}

} // namespace

void prepare(std::uint8_t *ram, std::size_t size) {
  if (!ram || size < 0x2000000) return;
  if (g_mode != Mode::main) return;
  const std::uint32_t owner = u32(ram, size, 0x00622F4C);
  const bool arcade = owner != 0 && u32(ram, size, owner + 0x3A3BC) == 0x61637261u;  // "arca"
  if (!arcade && !object(ram, size, g_box, kSelectBoxVtable)) return;
  if (g_ready && object(ram, size, g_box, kSelectBoxVtable) && entry_present(ram, size, g_box)) return;
  // A reloaded menu brings the templates back; start over.
  g_ready = false;
  g_entry = false;
  g_entry_index = -1;
  std::uint32_t box = 0, hidden = 0;
  if (!find_selectbox(ram, size, box)) return;
  if (!find_recomp(ram, size, hidden)) return;
  g_box = box;
  // Detach all 20 template rows; the host owns them from here.
  w32(ram, size, hidden + kRows, 0);
  w32(ram, size, hidden + kFirst, 0);
  w32(ram, size, hidden + kLast, 0);
  for (std::uint32_t &row : g_rows) w32(ram, size, row + kNext, 0);
  // Append the Recomp settings row after the original main-menu rows; menu_quit appends Quit after it.
  const std::uint32_t rows = u32(ram, size, g_box + kRows);
  const std::uint32_t last = u32(ram, size, g_box + kLast);
  const std::uint32_t entry = g_rows[kEntryRow];
  w32(ram, size, last + kNext, entry);
  w32(ram, size, entry + kParent, g_box);
  wf32(ram, size, entry + kRowGeometry, 64.0f);
  wf32(ram, size, entry + kRowGeometry + 4, 6.0f + 24.0f * static_cast<float>(rows));
  w32(ram, size, entry + kNext, 0);
  w32(ram, size, g_box + kLast, entry);
  w32(ram, size, g_box + kRows, rows + 1);
  w32(ram, size, g_box + kEnd, rows + 1);
  set_insensitive(ram, size, entry, false);
  g_ready = true;
  g_entry = true;
  // The launcher's renderer reaches the game as GT4_RENDERER (pcsx2 or hw); show the current one until the player
  // changes it. The render scale percentage is not passed to the game; the default 100% is shown until a change.
  if (const char *renderer = std::getenv("GT4_RENDERER")) g_renderer = std::strcmp(renderer, "hw") == 0 ? 1 : 0;
  std::fprintf(stderr, "[recomp-menu] Recomp settings row appended: %u rows, index %u; hidden recomp 0x%08X\n",
               rows + 1, rows, hidden);
}

void tick(std::uint8_t *ram, std::size_t size, std::uint16_t buttons) {
  if (!g_ready || !object(ram, size, g_box, kSelectBoxVtable)) {
    g_previous_buttons = buttons;
    return;
  }
  const bool cross = (buttons & kCross) != 0 && (g_previous_buttons & kCross) == 0;
  const bool circle = (buttons & kCircle) != 0 && (g_previous_buttons & kCircle) == 0;
  const bool up = (buttons & kUp) != 0 && (g_previous_buttons & kUp) == 0;
  const bool down = (buttons & kDown) != 0 && (g_previous_buttons & kDown) == 0;
  int index = static_cast<int>(u32(ram, size, g_box + kBoxIndex));
  if (g_mode == Mode::main) {
    g_current_index = index;
    g_entry_index = entry_present(ram, size, g_box) ? find_entry_index(ram, size, g_box) : -1;
    if (cross && g_entry_index >= 0 && index == g_entry_index) open_hub(ram, size);
  } else {
    // The host owns the focus list: move the index itself and keep it through the render hook. The game's SelectBox
    // update otherwise restores the index it knew.
    int next = g_current_index;
    if (up) --next;
    if (down) ++next;
    const int last = g_mode == Mode::hub ? 5 : (g_mode == Mode::graphics ? 2 : 0);
    if (next < 0) next = 0;
    if (next > last) next = last;
    if (next != g_current_index) {
      g_current_index = next;
      g_active.index = static_cast<std::uint32_t>(next);
      write_fields(ram, size, g_box, g_active);
      std::fprintf(stderr, "[recomp-menu] focus index %d (mode %d)\n", next, static_cast<int>(g_mode));
    }
    index = g_current_index;
    if (g_mode == Mode::hub) {
      if (circle) {
        back_main(ram, size);
      } else if (cross) {
        if (index >= 1 && index <= 5) {
          if (index == 1)
            open_graphics(ram, size);
          else if (index == 2)
            open_shell(ram, size, kControlsTitle);
          else if (index == 3)
            open_shell(ram, size, kDisplayTitle);
          else if (index == 4)
            open_shell(ram, size, kAudioTitle);
          else
            open_shell(ram, size, kSystemTitle);
        }
      }
    } else if (g_mode == Mode::graphics) {
      if (circle) {
        back_hub(ram, size);
      } else if (cross) {
        if (index == 1)
          toggle_renderer(ram, size);
        else if (index == 2)
          next_scale(ram, size);
      }
    } else if (g_mode == Mode::shell) {
      if (circle) back_hub(ram, size);
    }
  }
  g_previous_buttons = buttons;
}

bool host_menu_active() { return g_ready && g_mode != Mode::main; }

void on_render_frame(std::uint8_t *ram, std::size_t size) {
  if (!g_ready || g_mode == Mode::main || !g_active_valid) return;
  if (!object(ram, size, g_box, kSelectBoxVtable)) return;
  // After the page update and before the draw: the host index wins over the SelectBox's own restore.
  write_fields(ram, size, g_box, g_active);
}

std::uint16_t guest_buttons(std::uint16_t buttons) {
  if (!g_ready) return buttons;
  if (g_mode != Mode::main) return buttons & ~(kCross | kCircle | kUp | kDown);
  if (g_entry_index >= 0 && (g_current_index == g_entry_index || g_current_index == g_entry_index + 1))
    return buttons & ~(kCross | kCircle);
  return buttons;
}
void reset() {
  g_box = 0;
  for (std::uint32_t &row : g_rows) row = 0;
  g_ready = false;
  g_entry = false;
  g_entry_index = -1;
  g_mode = Mode::main;
  g_main = {};
  g_hub = {};
  g_active = {};
  g_active_valid = false;
  g_shell_title = -1;
  g_renderer = 0;
  g_scale = 2;
  g_renderer_changed = false;
  g_scale_changed = false;
  g_previous_buttons = 0;
  g_current_index = -1;
}

} // namespace gt4::recomp_menu
