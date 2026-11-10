// TASK-213 Phase 1d: the Screen page's Resolution and Monitor rows as the game's own OptionMenus.
// See native/screen_rows.hpp.
#include "screen_rows.hpp"

#include "game_target.hpp"
#include "native_settings.hpp"
#include "recomp_menu.hpp"
#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt4::screen_rows {
namespace {

struct Mode {
  int width = 0;
  int height = 0;
};

struct MonitorEntry {
  std::string device;  // \\.\DISPLAYn
  std::string label;   // friendly name, or the short device
  int width = 0;       // the monitor's current desktop mode
  int height = 0;
  bool primary = false;
};

// One popup choice: the game's TextFace and the text it currently shows.
struct Choice {
  std::uint32_t textface = 0;
  std::string text;
};

struct MenuRow {
  std::uint32_t row = 0;
  std::uint32_t menu = 0;
  std::uint32_t popup = 0;
  std::uint32_t vbox = 0;

  std::vector<Choice> choices;
  int confirmed = -1;   // the index the host last applied (or synced to); -1 until the first sync
  bool was_open = false;
  bool insensitive_set = false;
  bool insensitive = false;
};

struct PageRefs {
  std::uint32_t pane = 0;

  MenuRow resolution;
  MenuRow monitor;
};

std::vector<Mode> g_modes;
std::vector<MonitorEntry> g_monitors;
int g_mode_index = 0;
int g_monitor_index = 0;
int g_wanted_width = 0;
int g_wanted_height = 0;
std::string g_wanted_monitor;
bool g_configured = false;
PageRefs g_page;

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

bool geometry(const std::uint8_t *ram, std::size_t size, std::uint32_t at, float x, float y, float w, float h) {
  return f32(ram, size, at) == x && f32(ram, size, at + 4) == y && f32(ram, size, at + 8) == w &&
         f32(ram, size, at + 12) == h;
}

bool object(const std::uint8_t *ram, std::size_t size, std::uint32_t base, std::uint32_t vtable) {
  return base >= 0x100000 && base + 0x100 <= size && u32(ram, size, base + 4) == vtable;
}

std::uint32_t first_child(const std::uint8_t *ram, std::size_t size, std::uint32_t base) {
  return u32(ram, size, base + 0xA4);
}

std::uint32_t next_sibling(const std::uint8_t *ram, std::size_t size, std::uint32_t child) {
  return u32(ram, size, child + 0x5C);
}

std::vector<std::uint32_t> children(const std::uint8_t *ram, std::size_t size, std::uint32_t base) {
  std::vector<std::uint32_t> out;
  std::uint32_t child = first_child(ram, size, base);
  for (int i = 0; i < 128 && child; ++i) {
    if (child < 0x100000 || child + 0x100 > size) break;
    out.push_back(child);
    child = next_sibling(ram, size, child);
  }
  return out;
}

std::string wide_to_utf8(const wchar_t *text) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string out(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string &text) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (length <= 1) return {};
  std::wstring out(static_cast<std::size_t>(length - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
  return out;
}

std::string short_device(const std::string &device) {
  const std::size_t slash = device.rfind('\\');
  return slash == std::string::npos ? device : device.substr(slash + 1);
}

void add_monitor(const std::string &device, const std::string &label, int width, int height, bool primary) {
  for (const MonitorEntry &entry : g_monitors) {
    if (entry.device == device) return;
  }
  MonitorEntry entry;
  entry.device = device;
  entry.label = label.empty() ? short_device(device) : label;
  entry.width = width;
  entry.height = height;
  entry.primary = primary;
  g_monitors.push_back(entry);
}

BOOL CALLBACK monitor_callback(HMONITOR handle, HDC, LPRECT, LPARAM context) {
  auto *list = reinterpret_cast<bool *>(context);
  MONITORINFOEXW info{};
  info.cbSize = sizeof info;
  if (!GetMonitorInfoW(handle, &info)) return TRUE;
  const std::string device = wide_to_utf8(info.szDevice);
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  int width = 0, height = 0;
  if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    width = static_cast<int>(mode.dmPelsWidth);
    height = static_cast<int>(mode.dmPelsHeight);
  }
  std::string label;
  DISPLAY_DEVICEW display{};
  display.cb = sizeof display;
  if (EnumDisplayDevicesW(info.szDevice, 0, &display, 0) && display.DeviceString[0])
    label = wide_to_utf8(display.DeviceString);
  if (label.size() > 18) label = short_device(device);
  add_monitor(device, label, width, height, (info.dwFlags & MONITORINFOF_PRIMARY) != 0);
  if (list) *list = true;
  return TRUE;
}

void enumerate_monitors() {
  g_monitors.clear();
  bool any = false;
  EnumDisplayMonitors(nullptr, nullptr, monitor_callback, reinterpret_cast<LPARAM>(&any));
  if (g_monitors.empty()) {
    // Headless or no monitor API: fall back to the desktop mode as one entry.
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    int width = 1920, height = 1080;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
      width = static_cast<int>(mode.dmPelsWidth);
      height = static_cast<int>(mode.dmPelsHeight);
    }
    add_monitor("\\\\.\\DISPLAY1", "DISPLAY1", width, height, true);
  }
}

void enumerate_modes(const std::string &device) {
  g_modes.clear();
  const std::wstring wide = utf8_to_wide(device);
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  for (DWORD index = 0; index < 1024 && EnumDisplaySettingsW(wide.c_str(), index, &mode); ++index) {
    if (mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0) continue;
    bool known = false;
    for (const Mode &entry : g_modes)
      if (entry.width == static_cast<int>(mode.dmPelsWidth) && entry.height == static_cast<int>(mode.dmPelsHeight))
        known = true;
    if (!known) g_modes.push_back(Mode{static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight)});
  }
  std::sort(g_modes.begin(), g_modes.end(), [](const Mode &a, const Mode &b) {
    if (a.width != b.width) return a.width < b.width;
    return a.height < b.height;
  });
  if (g_modes.empty() && !g_monitors.empty()) {
    const MonitorEntry &monitor = g_monitors[static_cast<std::size_t>(g_monitor_index)];
    g_modes.push_back(Mode{monitor.width, monitor.height});
  }
}

int find_mode(int width, int height) {
  for (std::size_t i = 0; i < g_modes.size(); ++i)
    if (g_modes[i].width == width && g_modes[i].height == height) return static_cast<int>(i);
  return -1;
}

void configure_from_environment_impl() {
  g_wanted_width = 0;
  g_wanted_height = 0;
  g_wanted_monitor.clear();
  if (const char *text = std::getenv("GT4_OUTPUT_SIZE")) {
    int width = 0, height = 0;
    if (std::sscanf(text, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
      g_wanted_width = width;
      g_wanted_height = height;
    }
  }
  if (const char *text = std::getenv("GT4_MONITOR")) g_wanted_monitor = text;
  enumerate_monitors();
  g_monitor_index = 0;
  bool matched_monitor = false;
  for (std::size_t i = 0; i < g_monitors.size(); ++i) {
    if (!g_wanted_monitor.empty() && g_monitors[i].device == g_wanted_monitor) {
      g_monitor_index = static_cast<int>(i);
      matched_monitor = true;
    }
    if (!matched_monitor && g_monitors[i].primary) g_monitor_index = static_cast<int>(i);
  }
  const MonitorEntry &monitor = g_monitors[static_cast<std::size_t>(g_monitor_index)];
  enumerate_modes(monitor.device);
  g_mode_index = find_mode(g_wanted_width, g_wanted_height);
  if (g_mode_index < 0) g_mode_index = find_mode(monitor.width, monitor.height);
  if (g_mode_index < 0) g_mode_index = 0;
  g_configured = true;
  std::fprintf(stderr,
               "[screen-rows] resolution: %zu modes, current %dx%d; monitors: %zu, current %s (%dx%d)\n",
               g_modes.size(), g_modes.empty() ? 0 : g_modes[static_cast<std::size_t>(g_mode_index)].width,
               g_modes.empty() ? 0 : g_modes[static_cast<std::size_t>(g_mode_index)].height, g_monitors.size(),
               monitor.device.c_str(), monitor.width, monitor.height);
}

std::string mode_text(int index) {
  if (g_modes.empty()) return {};
  const Mode &mode = g_modes[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(g_modes.size()) - 1))];
  char text[32];
  std::snprintf(text, sizeof text, "%dx%d", mode.width, mode.height);
  return text;
}

std::string monitor_text(int index) {
  if (g_monitors.empty()) return {};
  const MonitorEntry &monitor =
      g_monitors[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(g_monitors.size()) - 1))];
  char text[64];
  std::snprintf(text, sizeof text, "%d: %s", index + 1, monitor.label.c_str());
  return text;
}

std::string monitor_id(int index) {
  if (g_monitors.empty()) return "primary";
  const MonitorEntry &monitor =
      g_monitors[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(g_monitors.size()) - 1))];
  char text[96];
  std::snprintf(text, sizeof text, "%s@%dx%d", monitor.device.c_str(), monitor.width, monitor.height);
  return text;
}

std::string toml_quote(const std::string &text) {
  std::string out = "\"";
  for (const char character : text) {
    switch (character) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += character;
    }
  }
  out += "\"";
  return out;
}

// Guest call with the caller's CPU state saved and restored: the string helpers must not disturb the main loop.
std::uint32_t call_guest(ps2::Cpu &cpu, std::uint32_t fn, std::uint32_t depth, std::uint32_t argc,
                         const std::uint32_t *args) {
  if (!fn || !ps2::has_code(fn)) return 0;
  const ps2::Cpu saved = cpu;
  for (std::uint32_t i = 0; i < argc && i < 4; ++i) cpu.r[4 + i].w[0] = args[i];
  cpu.r[31].w[0] = 0;
  ps2::call(cpu, fn, depth + 1u);
  const std::uint32_t result = cpu.r[2].w[0];
  cpu = saved;
  return result;
}

std::string read_display_text(const std::uint8_t *ram, std::size_t size, std::uint32_t textface) {
  const std::uint32_t localized = u32(ram, size, textface + gt4::target::active().hooks.screen_text_object_offset);
  if (!localized) return {};
  const std::uint32_t dat = u32(ram, size, localized + gt4::target::active().hooks.screen_text_value_offset);
  if (!dat || dat < 16 || dat >= size) return {};
  const std::uint32_t length = u32(ram, size, dat - 16);
  if (length > 256 || dat + length > size) return {};
  std::string out(reinterpret_cast<const char *>(ram + dat), length);
  const std::size_t nul = out.find('\0');
  if (nul != std::string::npos) out.resize(nul);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

void apply_insensitive(std::uint8_t *ram, std::size_t size, std::uint32_t widget, bool on, int depth) {
  const auto &hooks = gt4::target::active().hooks;
  if (!widget || depth > 16 || widget + 0x100 > size) return;
  const std::uint32_t vtable = u32(ram, size, widget + 4);
  if (vtable < 0x00600000 || vtable >= 0x00700000) return;
  const std::uint32_t at = widget + hooks.screen_insensitive_offset;
  if (at + 4 <= size) {
    std::uint32_t word = u32(ram, size, at);
    if (on)
      word |= 1u << hooks.screen_insensitive_bit;
    else
      word &= ~(1u << hooks.screen_insensitive_bit);
    w32(ram, size, at, word);
  }
  // Only real composites carry a child list at +0xA4; a leaf's +0xA4 is another field.
  if (vtable != hooks.screen_composite_vtable && vtable != hooks.screen_pane_vtable &&
      vtable != hooks.screen_slider_vtable && vtable != hooks.screen_optionmenu_vtable)
    return;
  std::uint32_t child = first_child(ram, size, widget);
  for (int i = 0; i < 128 && child; ++i) {
    apply_insensitive(ram, size, child, on, depth + 1);
    child = next_sibling(ram, size, child);
  }
}

void set_row_insensitive(std::uint8_t *ram, std::size_t size, MenuRow &row, bool on) {
  if (row.insensitive_set && row.insensitive == on) return;
  apply_insensitive(ram, size, row.menu, on, 0);
  row.insensitive_set = true;
  row.insensitive = on;
  std::fprintf(stderr, "[screen-rows] %s row insensitive=%d\n", on ? "greyed" : "active", on ? 1 : 0);
}

bool read_menu_choices(const std::uint8_t *ram, std::size_t size, MenuRow &row) {
  const auto &hooks = gt4::target::active().hooks;
  row.popup = u32(ram, size, row.menu + hooks.screen_menu_popup_offset);
  row.vbox = u32(ram, size, row.menu + hooks.screen_menu_vbox_offset);
  if (!row.vbox || row.vbox + 0x100 > size) return false;
  row.choices.clear();
  for (const std::uint32_t child : children(ram, size, row.vbox)) {
    if (!object(ram, size, child, hooks.screen_composite_vtable)) continue;
    for (const std::uint32_t item : children(ram, size, child)) {
      if (!object(ram, size, item, hooks.screen_textface_vtable)) continue;
      std::string text = read_display_text(ram, size, item);
      if (text.empty()) continue;
      row.choices.push_back(Choice{item, text});
      break;
    }
  }
  return !row.choices.empty();
}

bool find_menu_row(const std::uint8_t *ram, std::size_t size, std::uint32_t pane, float y, MenuRow &row) {
  const auto &hooks = gt4::target::active().hooks;
  for (const std::uint32_t child : children(ram, size, pane)) {
    if (!object(ram, size, child, hooks.screen_composite_vtable)) continue;
    if (f32(ram, size, child + 0x34 + 4) != y) continue;
    row.row = child;
    for (const std::uint32_t item : children(ram, size, child)) {
      if (object(ram, size, item, hooks.screen_optionmenu_vtable)) row.menu = item;
    }
    if (!row.menu) continue;
    if (!read_menu_choices(ram, size, row)) continue;
    return true;
  }
  return false;
}

bool pane_geometry(const std::uint8_t *ram, std::size_t size, std::uint32_t pane) {
  // The patched page is 320 x 214 (three setting rows); the original 150 is accepted so a stock page is not mistaken
  // for another page. The rows are found by their OptionMenu children, not by the height.
  return f32(ram, size, pane + 0x34) == 160.0f && f32(ram, size, pane + 0x34 + 4) == 130.0f &&
         f32(ram, size, pane + 0x34 + 8) == 320.0f &&
         (f32(ram, size, pane + 0x34 + 12) == 214.0f || f32(ram, size, pane + 0x34 + 12) == 150.0f);
}

bool find_page(const std::uint8_t *ram, std::size_t size) {
  const auto &hooks = gt4::target::active().hooks;
  for (std::uint32_t at = 0x100000; at + 0x100 < size; at += 4) {
    if (u32(ram, size, at) != hooks.screen_pane_vtable) continue;
    const std::uint32_t pane = at - 4;
    if (!pane_geometry(ram, size, pane)) continue;
    PageRefs page;
    page.pane = pane;
    // The Wide/Fullscreen row is at y = 20; the two host rows are the OptionMenu rows below it.
    if (!find_menu_row(ram, size, pane, 52.0f, page.resolution)) continue;
    if (!find_menu_row(ram, size, pane, 84.0f, page.monitor)) continue;
    g_page = page;
    std::fprintf(stderr,
                 "[screen-rows] Screen page 0x%08X: resolution menu 0x%08X (%zu choices), monitor menu 0x%08X (%zu)\n",
                 pane, page.resolution.menu, page.resolution.choices.size(), page.monitor.menu,
                 page.monitor.choices.size());
    return true;
  }
  return false;
}

bool page_valid(const std::uint8_t *ram, std::size_t size) {
  const auto &hooks = gt4::target::active().hooks;
  return g_page.pane && object(ram, size, g_page.pane, hooks.screen_pane_vtable) &&
         object(ram, size, g_page.resolution.menu, hooks.screen_optionmenu_vtable) &&
         object(ram, size, g_page.monitor.menu, hooks.screen_optionmenu_vtable);
}


void sync_resolution(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, MenuRow &row) {
  (void)cpu;
  (void)depth;
  const auto &hooks = gt4::target::active().hooks;
  const std::string wanted = mode_text(g_mode_index);
  int index = 0;
  for (std::size_t i = 0; i < row.choices.size(); ++i) {
    if (row.choices[i].text == wanted) {
      index = static_cast<int>(i);
      break;
    }
  }
  w32(ram, size, row.menu + hooks.screen_menu_index_offset, static_cast<std::uint32_t>(index));
  row.confirmed = index;
  std::fprintf(stderr, "[screen-rows] resolution menu synced to \"%s\" (index %d of %zu)\n", wanted.c_str(), index,
               row.choices.size());
}

void sync_monitor(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, MenuRow &row) {
  (void)cpu;
  (void)depth;
  const auto &hooks = gt4::target::active().hooks;
  const int index = std::clamp(g_monitor_index, 0, static_cast<int>(row.choices.size()) - 1);
  w32(ram, size, row.menu + hooks.screen_menu_index_offset, static_cast<std::uint32_t>(index));
  row.confirmed = index;
  std::fprintf(stderr, "[screen-rows] monitor menu synced to \"%s\" (index %d of %zu)\n",
               monitor_text(g_monitor_index).c_str(), index, row.choices.size());
}

void write_resolution_feedback(const std::string &value, const char *source) {
  const std::string quoted = toml_quote(value);
  const char *keys[1] = {"resolution"};
  const char *values[1] = {quoted.c_str()};
  char error[160] = {};
  if (gt4::native_settings::write_feedback_changes(keys, values, 1, error, sizeof error))
    std::fprintf(stderr, "[screen-rows] %s: resolution = %s\n", source, quoted.c_str());
  else
    std::fprintf(stderr, "[screen-rows] %s: feedback not written: %s\n", source, error);
}

void write_monitor_feedback(int index, const char *source) {
  const std::string value = toml_quote(monitor_id(index));
  const char *keys[1] = {"monitor"};
  const char *values[1] = {value.c_str()};
  char error[160] = {};
  if (gt4::native_settings::write_feedback_changes(keys, values, 1, error, sizeof error))
    std::fprintf(stderr, "[screen-rows] %s: monitor = %s (restart)\n", source, value.c_str());
  else
    std::fprintf(stderr, "[screen-rows] %s: feedback not written: %s\n", source, error);
}

void apply_resolution(const MenuRow &row, int index) {
  if (index < 0 || index >= static_cast<int>(row.choices.size())) return;
  const std::string &text = row.choices[static_cast<std::size_t>(index)].text;
  int width = 0, height = 0;
  if (std::sscanf(text.c_str(), "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
    std::fprintf(stderr, "[screen-rows] resolution choice \"%s\" is not WxH; ignored\n", text.c_str());
    return;
  }
  const int host_index = find_mode(width, height);
  if (host_index >= 0) g_mode_index = host_index;
  write_resolution_feedback(text, "dropdown");
  if (gt4::native_settings::window_present()) {
    gt4::native_settings::request_output_size(width, height);
    std::fprintf(stderr, "[screen-rows] resolution %s -> live apply requested (mode list %s)\n", text.c_str(),
                 host_index >= 0 ? "has it" : "does not list it");
  } else {
    std::fprintf(stderr, "[screen-rows] resolution %s: this run has no window; the value applies at the next start\n",
                 text.c_str());
  }
}

void apply_monitor(const MenuRow &row, int index) {
  if (index < 0 || index >= static_cast<int>(row.choices.size())) return;
  const int monitor = std::clamp(index, 0, static_cast<int>(g_monitors.size()) - 1);
  g_monitor_index = monitor;
  write_monitor_feedback(monitor, "dropdown");
  std::fprintf(stderr, "[screen-rows] monitor %s: restart value (moving a running window is not implemented)\n",
               monitor_text(monitor).c_str());
}

void service_row(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, MenuRow &row, int which) {
  const auto &hooks = gt4::target::active().hooks;
  if (!row.menu || row.choices.empty()) return;
  set_row_insensitive(ram, size, row, !gt4::native_settings::host_fullscreen());
  const std::uint32_t raw = u32(ram, size, row.menu + hooks.screen_menu_index_offset);
  const int index = std::clamp(static_cast<int>(raw), 0, static_cast<int>(row.choices.size()) - 1);
  const bool open = u32(ram, size, row.menu + hooks.screen_menu_open_offset) != 0;
  if (row.confirmed < 0) {
    // First sight: show the current value from the host (and paint the monitor names once).
    if (which == 0)
      sync_resolution(cpu, ram, size, depth, row);
    else
      sync_monitor(cpu, ram, size, depth, row);
    row.was_open = open;
    return;
  }
  if (open) {
    row.was_open = true;
    return;
  }
  if (index != row.confirmed && !row.was_open) {
    // The index moved with the popup closed (a programmatic change or a first sync); apply it like a confirm.
    row.confirmed = index;
    if (which == 0)
      apply_resolution(row, index);
    else
      apply_monitor(row, index);
    return;
  }
  if (row.was_open) {
    row.was_open = false;
    if (index != row.confirmed) {
      row.confirmed = index;
      if (which == 0)
        apply_resolution(row, index);
      else
        apply_monitor(row, index);
    }
  }
}


} // namespace

std::uint32_t adjust_v_entry() { return gt4::target::active().hooks.screen_adjust_v_callback; }
std::uint32_t adjust_h_entry() { return gt4::target::active().hooks.screen_adjust_h_callback; }
std::uint32_t render_entry() { return gt4::target::active().hooks.screen_render_functor; }

void configure_from_environment() {
  if (g_configured) return;
  configure_from_environment_impl();
}

bool on_adjust(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, int which) {
  // The stock `Vertical`/`Horizontal` SliderBars are hidden by the data patch; the page script still calls their
  // getters/setters in reflect/apply. The host must keep both out of the guest save: the getter returns 0 and the setter
  // is skipped. The visible rows are the OptionMenus serviced in on_render_frame.
  const auto &hooks = gt4::target::active().hooks;
  const std::uint32_t entry = which == 0 ? hooks.screen_adjust_v_callback : hooks.screen_adjust_h_callback;
  if (!entry || !ram || !gt4::native_settings::option_project_active(ram, size)) return false;
  const std::uint32_t return_label = cpu.r[31].w[0];
  if (cpu.r[6].w[0] > 0) {
    cpu.pc = return_label;
    return true;
  }
  const std::uint32_t slot = cpu.r[4].w[0];
  if (!slot || slot + 4 > size) return false;
  if (!hooks.script_box_int || !ps2::has_code(hooks.script_box_int)) return false;
  const std::uint32_t old = u32(ram, size, slot);
  if (old) {
    if (!hooks.script_release_handle || !ps2::has_code(hooks.script_release_handle)) return false;
    const std::uint32_t args[1] = {old};
    call_guest(cpu, hooks.script_release_handle, depth, 1, args);
  }
  const std::uint32_t args[2] = {slot, 0u};
  call_guest(cpu, hooks.script_box_int, depth, 2, args);
  cpu.pc = return_label;
  return true;
}

void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth) {
  if (!ram) return;
  // TASK-213 Phase 2a: keep the Recomp settings SelectBox focus through the page update (arcade main menu).
  gt4::recomp_menu::on_render_frame(ram, size);
  if (!gt4::native_settings::option_project_active(ram, size)) return;
  if (!g_configured) configure_from_environment();
  if (!page_valid(ram, size) && !find_page(ram, size)) return;
  service_row(cpu, ram, size, depth, g_page.resolution, 0);
  service_row(cpu, ram, size, depth, g_page.monitor, 1);
}

void reset_for_test() {
  g_modes.clear();
  g_monitors.clear();
  g_mode_index = 0;
  g_monitor_index = 0;
  g_wanted_width = 0;
  g_wanted_height = 0;
  g_wanted_monitor.clear();
  g_configured = false;
  g_page = PageRefs{};
}

} // namespace gt4::screen_rows
