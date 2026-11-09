// TASK-213 Phase 1c: the Screen page's Resolution and Monitor rows. See native/screen_rows.hpp.
#include "screen_rows.hpp"

#include "game_target.hpp"
#include "native_settings.hpp"
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

struct RowRefs {
  std::uint32_t row = 0;
  std::uint32_t slider = 0;
  std::uint32_t value = 0;
  std::uint32_t pinch = 0;
  std::uint32_t min = 0;
  std::uint32_t max = 0;
  bool insensitive_set = false;
  bool insensitive = false;
};

struct PageRefs {
  std::uint32_t pane = 0;
  RowRefs resolution;
  RowRefs monitor;
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

// One guest basic_string per list entry, built once when the page is first seen. The per-frame service only swaps the
// TextFace's text pointer (memory, no guest calls): building a string on the render frame while the page script was
// writing its number crashed in the allocator lock (0xC0000005 in kernel::handle_syscall), so no build ever runs there.
struct Prebuilt {
  bool ready = false;
  std::uint32_t zero = 0;
  std::uint32_t mode_max = 0;
  std::uint32_t monitor_max = 0;
  std::vector<std::uint32_t> modes;
  std::vector<std::uint32_t> monitors;
};

Prebuilt g_prebuilt;

std::uint32_t build_guest_string(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth,
                                 std::uint32_t seed, const std::string &text) {
  const auto &hooks = gt4::target::active().hooks;
  if (!hooks.screen_text_assign || !hooks.screen_heap_name || !hooks.screen_alloc || !hooks.screen_free) return 0;
  const std::uint32_t info = call_guest(cpu, hooks.screen_heap_name, depth, 0, nullptr);
  if (!info) return 0;
  const std::uint32_t name = u32(ram, size, info);
  const std::uint32_t length = static_cast<std::uint32_t>(text.size());
  const std::uint32_t buffer_bytes = length + 96;
  const std::uint32_t args0[3] = {buffer_bytes, 4u, name};
  const std::uint32_t buffer = call_guest(cpu, hooks.screen_alloc, depth, 3, args0);
  if (!buffer || buffer + buffer_bytes > size) return 0;
  std::memcpy(ram + buffer, text.data(), length);
  ram[buffer + length] = 0;
  const std::uint32_t scratch = (buffer + length + 16u + 3u) & ~3u;
  w32(ram, size, scratch, seed);
  if (seed) {
    const std::uint32_t seed_rep = seed - 16;
    if (seed >= 16 && seed_rep + 12 <= size)
      w32(ram, size, seed_rep + 8, u32(ram, size, seed_rep + 8) + 1u);
  }
  const std::uint32_t args1[5] = {scratch, 0u, 0xFFFFFFFFu, buffer, length};
  call_guest(cpu, hooks.screen_text_assign, depth, 5, args1);
  const std::uint32_t fresh = u32(ram, size, scratch);
  const std::uint32_t free_args[4] = {buffer, buffer_bytes, 4u, name};
  call_guest(cpu, hooks.screen_free, depth, 4, free_args);
  // The fresh string keeps the scratch's reference as this cache's own; it is never freed (a few hundred bytes per run).
  return fresh;
}

void point_text(std::uint8_t *ram, std::size_t size, std::uint32_t textface, std::uint32_t dat, const char *what,
                const std::string &text) {
  if (!textface || !dat) return;
  const std::uint32_t localized = u32(ram, size, textface + gt4::target::active().hooks.screen_text_object_offset);
  if (!localized) return;
  const std::uint32_t slot = localized + gt4::target::active().hooks.screen_text_value_offset;
  if (u32(ram, size, slot) == dat) return;
  const std::uint32_t rep = dat - 16;
  if (rep + 12 > size) return;
  w32(ram, size, rep + 8, u32(ram, size, rep + 8) + 1u);
  w32(ram, size, slot, dat);
  std::fprintf(stderr, "[screen-rows] %s -> \"%s\" (frame %llu)\n", what, text.c_str(),
               static_cast<unsigned long long>(ps2::frame_count()));
}

void ensure_prebuilt(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth) {
  if (g_prebuilt.ready) return;
  if (g_modes.empty() && g_monitors.empty()) return;
  const std::uint32_t localized =
      u32(ram, size, g_page.resolution.value + gt4::target::active().hooks.screen_text_object_offset);
  const std::uint32_t seed =
      localized ? u32(ram, size, localized + gt4::target::active().hooks.screen_text_value_offset) : 0;
  for (std::size_t i = 0; i < g_modes.size(); ++i) g_prebuilt.modes.push_back(build_guest_string(cpu, ram, size, depth, seed, mode_text(static_cast<int>(i))));
  for (std::size_t i = 0; i < g_monitors.size(); ++i) g_prebuilt.monitors.push_back(build_guest_string(cpu, ram, size, depth, seed, monitor_text(static_cast<int>(i))));
  g_prebuilt.zero = build_guest_string(cpu, ram, size, depth, seed, "0");
  char bounds[16];
  std::snprintf(bounds, sizeof bounds, "%d", g_modes.empty() ? 0 : static_cast<int>(g_modes.size()) - 1);
  g_prebuilt.mode_max = build_guest_string(cpu, ram, size, depth, seed, bounds);
  std::snprintf(bounds, sizeof bounds, "%d", g_monitors.empty() ? 0 : static_cast<int>(g_monitors.size()) - 1);
  g_prebuilt.monitor_max = build_guest_string(cpu, ram, size, depth, seed, bounds);
  g_prebuilt.ready = true;
  std::fprintf(stderr, "[screen-rows] prebuilt %zu mode names, %zu monitor names (seed 0x%08X)\n",
               g_prebuilt.modes.size(), g_prebuilt.monitors.size(), seed);
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
      vtable != hooks.screen_slider_vtable)
    return;
  std::uint32_t child = first_child(ram, size, widget);
  for (int i = 0; i < 48 && child; ++i) {
    apply_insensitive(ram, size, child, on, depth + 1);
    child = next_sibling(ram, size, child);
  }
}

void set_row_insensitive(std::uint8_t *ram, std::size_t size, RowRefs &row, bool on) {
  if (row.insensitive_set && row.insensitive == on) return;
  apply_insensitive(ram, size, row.slider, on, 0);
  row.insensitive_set = true;
  row.insensitive = on;
  std::fprintf(stderr, "[screen-rows] row %s insensitive=%d\n", on ? "greyed" : "active", on ? 1 : 0);
}

bool page_valid(const std::uint8_t *ram, std::size_t size) {
  const auto &hooks = gt4::target::active().hooks;
  return g_page.pane && object(ram, size, g_page.pane, hooks.screen_pane_vtable) &&
         object(ram, size, g_page.resolution.slider, hooks.screen_slider_vtable) &&
         object(ram, size, g_page.monitor.slider, hooks.screen_slider_vtable);
}

bool find_row(const std::uint8_t *ram, std::size_t size, std::uint32_t pane, float y, RowRefs &row) {
  const auto &hooks = gt4::target::active().hooks;
  for (std::uint32_t child = first_child(ram, size, pane); child; child = next_sibling(ram, size, child)) {
    if (!object(ram, size, child, hooks.screen_composite_vtable)) continue;
    if (!geometry(ram, size, child + 0x34, 0.0f, y, 320.0f, 32.0f)) continue;
    row.row = child;
    for (std::uint32_t item = first_child(ram, size, child); item; item = next_sibling(ram, size, item)) {
      if (object(ram, size, item, hooks.screen_slider_vtable)) row.slider = item;
    }
    if (!row.slider) continue;
    for (std::uint32_t item = first_child(ram, size, row.slider); item; item = next_sibling(ram, size, item)) {
      if (!row.value && object(ram, size, item, hooks.screen_textface_vtable)) row.value = item;
      if (!row.pinch && object(ram, size, item, hooks.screen_composite_vtable) &&
          geometry(ram, size, item + 0x34, 14.0f, 0.0f, 132.0f, 24.0f)) {

        for (std::uint32_t popup = first_child(ram, size, item); popup;
             popup = next_sibling(ram, size, popup)) {
          if (object(ram, size, popup, hooks.screen_textface_vtable)) {
            if (!row.min) row.min = popup;
            else if (!row.max) row.max = popup;

          } else if (object(ram, size, popup, hooks.screen_composite_vtable)) {
            for (std::uint32_t pinch = first_child(ram, size, popup); pinch;
                 pinch = next_sibling(ram, size, pinch)) {
              if (object(ram, size, pinch, hooks.screen_textface_vtable)) row.pinch = pinch;
            }
          }
        }

      }
    }
    return row.slider && row.value;
  }
  return false;
}

bool find_page(const std::uint8_t *ram, std::size_t size) {
  const auto &hooks = gt4::target::active().hooks;
  for (std::uint32_t at = 0x100000; at + 0x100 < size; at += 4) {
    if (u32(ram, size, at) != hooks.screen_pane_vtable) continue;
    const std::uint32_t pane = at - 4;
    if (!geometry(ram, size, pane + 0x34, 160.0f, 130.0f, 320.0f, 150.0f)) continue;
    PageRefs page;
    page.pane = pane;
    if (!find_row(ram, size, pane, 52.0f, page.resolution)) continue;
    if (!find_row(ram, size, pane, 84.0f, page.monitor)) continue;
    g_page = page;
    std::fprintf(stderr, "[screen-rows] Screen page 0x%08X: resolution slider 0x%08X, monitor slider 0x%08X\n", pane,
                 page.resolution.slider, page.monitor.slider);
    return true;
  }
  return false;
}

bool text_enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("GT4_SCREEN_ROWS_TEXT");
    return value == nullptr || *value != '0';
  }();
  return enabled;
}

bool insensitive_enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("GT4_SCREEN_ROWS_INSENSITIVE");
    return value == nullptr || *value != '0';
  }();
  return enabled;
}

void service_row(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth, RowRefs &row, int which) {
  if (!g_prebuilt.ready) ensure_prebuilt(cpu, ram, size, depth);
  const std::string value = which == 0 ? mode_text(g_mode_index) : monitor_text(g_monitor_index);
  const std::uint32_t value_dat =
      which == 0
          ? (g_prebuilt.modes.empty() ? 0 : g_prebuilt.modes[static_cast<std::size_t>(g_mode_index)])
          : (g_prebuilt.monitors.empty() ? 0 : g_prebuilt.monitors[static_cast<std::size_t>(g_monitor_index)]);
  const std::uint32_t max_dat = which == 0 ? g_prebuilt.mode_max : g_prebuilt.monitor_max;
  if (text_enabled() && value_dat) {
    point_text(ram, size, row.value, value_dat, which == 0 ? "resolution" : "monitor", value);
    point_text(ram, size, row.pinch, value_dat, which == 0 ? "resolution popup" : "monitor popup", value);
    point_text(ram, size, row.min, g_prebuilt.zero, "min", "0");
    point_text(ram, size, row.max, max_dat, "max", "list max");
  }
  // The owner's rule: Resolution/Monitor are greyed and not selectable while Fullscreen is Off. The name stays visible.
  if (insensitive_enabled()) set_row_insensitive(ram, size, row, !gt4::native_settings::host_fullscreen());
}

void write_row_feedback(int which, const char *source) {
  if (which == 0 && g_modes.empty()) return;
  if (which == 1 && g_monitors.empty()) return;
  const char *key = which == 0 ? "resolution" : "monitor";
  const std::string value = which == 0 ? mode_text(g_mode_index) : monitor_id(g_monitor_index);
  const std::string quoted = toml_quote(value);
  const char *keys[1] = {key};
  const char *values[1] = {quoted.c_str()};
  char error[160] = {};
  if (gt4::native_settings::write_feedback_changes(keys, values, 1, error, sizeof error))
    std::fprintf(stderr, "[screen-rows] %s: %s = %s (restart)\n", source, key, quoted.c_str());
  else
    std::fprintf(stderr, "[screen-rows] %s: feedback not written: %s\n", source, error);
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
  const auto &hooks = gt4::target::active().hooks;
  const std::uint32_t entry = which == 0 ? hooks.screen_adjust_v_callback : hooks.screen_adjust_h_callback;
  if (!entry || !ram || !gt4::native_settings::option_project_active(ram, size)) return false;
  if (!g_configured) configure_from_environment();
  const std::uint32_t return_label = cpu.r[31].w[0];
  const int count = which == 0 ? static_cast<int>(g_modes.size()) : static_cast<int>(g_monitors.size());
  if (count <= 0) return false;
  int &index = which == 0 ? g_mode_index : g_monitor_index;
  if (cpu.r[6].w[0] > 0) {
    std::int32_t raw = 0;
    if (!gt4::native_settings::read_hint_value(ram, size, cpu.r[7].w[0], raw)) return false;
    const int clamped = std::clamp(static_cast<int>(raw), 0, count - 1);
    if (clamped != index) {
      index = clamped;
      write_row_feedback(which, "row");
    }
    cpu.pc = return_label;
    return true;
  }
  // Getter: box the host index into the caller's return slot.
  const std::uint32_t slot = cpu.r[4].w[0];
  if (!slot || slot + 4 > size) return false;
  if (!hooks.script_box_int || !ps2::has_code(hooks.script_box_int)) return false;
  const std::uint32_t old = u32(ram, size, slot);
  if (old) {
    if (!hooks.script_release_handle || !ps2::has_code(hooks.script_release_handle)) return false;
    const std::uint32_t args[1] = {old};
    call_guest(cpu, hooks.script_release_handle, depth, 1, args);
  }
  const std::uint32_t args[2] = {slot, static_cast<std::uint32_t>(index)};
  call_guest(cpu, hooks.script_box_int, depth, 2, args);
  cpu.pc = return_label;
  return true;
}

void on_render_frame(ps2::Cpu &cpu, std::uint8_t *ram, std::size_t size, std::uint32_t depth) {
  if (!ram || !gt4::native_settings::option_project_active(ram, size)) return;
  if (!g_configured) configure_from_environment();
  if (g_modes.empty() && g_monitors.empty()) return;
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
  g_prebuilt = Prebuilt{};
}

} // namespace gt4::screen_rows
