#include "wheel.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace gt4::wheel {

namespace {

[[noreturn]] void fail(const std::string &source, std::size_t line, const std::string &message) {
  throw ConfigError(source + ":" + std::to_string(line) + ": " + message);
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

std::string to_lower_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return out;
}

std::string_view strip_comment(std::string_view line, const std::string &source, std::size_t line_no) {
  bool in_quotes = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (line[index] == '"') {
      if (index > 0 && line[index - 1] == '\\')
        fail(source, line_no, "escape sequences are not supported in wheel strings");
      in_quotes = !in_quotes;
    } else if (line[index] == '#' && !in_quotes) {
      return line.substr(0, index);
    }
  }
  if (in_quotes)
    fail(source, line_no, "unterminated quoted string");
  return line;
}

std::string_view unquote(std::string_view token, const std::string &source, std::size_t line_no) {
  token = trim(token);
  if (token.size() < 2 || token.front() != '"' || token.back() != '"')
    fail(source, line_no, "expected a double-quoted string");
  std::string_view inner = token.substr(1, token.size() - 2);
  if (inner.find('"') != std::string_view::npos)
    fail(source, line_no, "unexpected quote inside string");
  return inner;
}

bool parse_bool_token(std::string_view token, const std::string &source, std::size_t line_no) {
  token = trim(token);
  if (token == "true")
    return true;
  if (token == "false")
    return false;
  fail(source, line_no, "expected true or false");
}

int parse_int_token(std::string_view token, const std::string &source, std::size_t line_no, const char *label) {
  token = trim(token);
  int value = 0;
  const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (ec != std::errc{} || ptr != token.data() + token.size())
    fail(source, line_no, std::string("invalid integer for ") + label);
  return value;
}

double parse_double_token(std::string_view token, const std::string &source, std::size_t line_no, const char *label) {
  token = trim(token);
  if (token.empty())
    fail(source, line_no, std::string("missing number for ") + label);
  std::string copy(token);
  char *end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(value))
    fail(source, line_no, std::string("invalid number for ") + label);
  return value;
}

Curve parse_curve_token(std::string_view token, const std::string &source, std::size_t line_no) {
  if (token == "linear")
    return Curve::linear;
  if (token == "quadratic")
    return Curve::quadratic;
  if (token == "cubic")
    return Curve::cubic;
  fail(source, line_no, "unknown curve '" + std::string(token) + "' (expected linear, quadratic, or cubic)");
}

// Pedal travel shape. The wheel file only offers linear, quadratic and cubic; the other shapes of the controls file
// (native/controls.cpp) are listed so every value of the shared enum is handled.
double apply_curve(Curve curve, double value) {
  switch (curve) {
  case Curve::linear:
    return value;
  case Curve::quadratic:
    return value * value;
  case Curve::cubic:
    return value * value * value;
  case Curve::s_curve:
    return value * value * (3.0 - 2.0 * value);
  case Curve::smooth_power:
    return value * std::sqrt(value);
  case Curve::sublinear:
    return std::sqrt(value);
  case Curve::logarithmic:
    return std::log(1.0 + 9.0 * value) / std::log(10.0);
  }
  return value;
}

bool is_valid_guid_string(std::string_view guid) {
  if (guid.size() != 32)
    return false;
  for (char ch : guid) {
    const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!hex)
      return false;
  }
  return true;
}

std::uint8_t parse_u8_index(std::string_view token, std::uint8_t max_val, const std::string &source,
                            std::size_t line_no, const char *label) {
  int val = parse_int_token(token, source, line_no, label);
  if (val < 0 || val > static_cast<int>(max_val))
    fail(source, line_no, std::string(label) + " index out of range (0.." + std::to_string(max_val) + ")");
  return static_cast<std::uint8_t>(val);
}

std::vector<std::string_view> parse_string_list(std::string_view raw, const std::string &source, std::size_t line_no) {
  raw = trim(raw);
  if (!raw.empty() && raw.front() == '"')
    return {unquote(raw, source, line_no)};
  if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']')
    fail(source, line_no, "expected an array of strings or a quoted string");
  std::string_view body = trim(raw.substr(1, raw.size() - 2));
  if (body.empty())
    return {};
  std::vector<std::string_view> items;
  std::size_t pos = 0;
  while (pos < body.size()) {
    bool in_quotes = false;
    std::size_t comma = body.size();
    for (std::size_t i = pos; i < body.size(); ++i) {
      if (body[i] == '"')
        in_quotes = !in_quotes;
      else if (body[i] == ',' && !in_quotes) {
        comma = i;
        break;
      }
    }
    std::string_view item = trim(body.substr(pos, comma - pos));
    if (item.empty())
      fail(source, line_no, "empty element in array");
    items.push_back(unquote(item, source, line_no));
    if (comma == body.size())
      break;
    pos = comma + 1;
    if (pos == body.size())
      fail(source, line_no, "trailing comma in array");
  }
  return items;
}

std::optional<std::size_t> slot_index(std::string_view name) {
  for (std::size_t i = 0; i < button_slots.size(); ++i)
    if (name == button_slots[i].name)
      return i;
  return std::nullopt;
}

std::optional<std::size_t> find_button_slot(std::string_view key) {
  if (const auto slot = slot_index(key))
    return slot;
  // Friendly aliases accepted in wheel.toml [buttons] (1.2.x files).
  if (key == "shift_up" || key == "gear_up" || key == "paddle_right")
    return slot_index("r2"); // the DFP's R1
  if (key == "shift_down" || key == "gear_down" || key == "paddle_left")
    return slot_index("l2"); // the DFP's L1
  if (key == "look_back" || key == "rear_view")
    return slot_index("l1");
  if (key == "nitro" || key == "boost" || key == "high_beam")
    return slot_index("r1");
  if (key == "reverse")
    return slot_index("triangle");
  if (key == "handbrake_btn" || key == "ebrake")
    return slot_index("circle");
  if (key == "camera" || key == "view" || key == "change_view")
    return slot_index("select");
  if (key == "pause")
    return slot_index("start");
  if (key == "horn")
    return slot_index("l3");
  return std::nullopt;
}

void set_slot(WheelConfig &config, const char *name, SourceRef ref) { config.buttons[*slot_index(name)] = {std::move(ref)}; }

// Generic bindings: buttons 0..11 in the order of a DFP-like wheel (the Logitech G29/G923 PlayStation layout), hat 0.
void assign_default_buttons(WheelConfig &config) {
  for (auto &slot : config.buttons)
    slot.clear();
  set_slot(config, "dpad_up", {SourceKind::hat, {}, 0, HatDirection::up});
  set_slot(config, "dpad_right", {SourceKind::hat, {}, 0, HatDirection::right});
  set_slot(config, "dpad_down", {SourceKind::hat, {}, 0, HatDirection::down});
  set_slot(config, "dpad_left", {SourceKind::hat, {}, 0, HatDirection::left});
  set_slot(config, "cross", {SourceKind::button, {}, 0, HatDirection::up});
  set_slot(config, "square", {SourceKind::button, {}, 1, HatDirection::up});
  set_slot(config, "circle", {SourceKind::button, {}, 2, HatDirection::up});
  set_slot(config, "triangle", {SourceKind::button, {}, 3, HatDirection::up});
  set_slot(config, "r2", {SourceKind::button, {}, 4, HatDirection::up}); // right paddle: gear up (DFP R1)
  set_slot(config, "l2", {SourceKind::button, {}, 5, HatDirection::up}); // left paddle: gear down (DFP L1)
  set_slot(config, "r1", {SourceKind::button, {}, 6, HatDirection::up}); // DFP R2
  set_slot(config, "l1", {SourceKind::button, {}, 7, HatDirection::up}); // DFP L2
  set_slot(config, "select", {SourceKind::button, {}, 8, HatDirection::up});
  set_slot(config, "start", {SourceKind::button, {}, 9, HatDirection::up});
  set_slot(config, "r3", {SourceKind::button, {}, 10, HatDirection::up});
  set_slot(config, "l3", {SourceKind::button, {}, 11, HatDirection::up});
}

const DeviceReading *resolve_device(const SourceRef &ref, const std::vector<DeviceReading> &devices, int primary_idx) {
  if (ref.empty())
    return nullptr;
  if (!ref.device_guid.empty()) {
    const std::string target = to_lower_ascii(ref.device_guid);
    const DeviceReading *match = nullptr;
    for (const auto &device : devices) {
      if (to_lower_ascii(device.guid) == target) {
        if (match)
          return nullptr; // Identical model GUIDs cannot identify a unique physical device.
        match = &device;
      }
    }
    return match;
  }
  if (primary_idx >= 0 && static_cast<std::size_t>(primary_idx) < devices.size())
    return &devices[static_cast<std::size_t>(primary_idx)];
  return nullptr;
}

bool evaluate_digital_source(const SourceRef &ref, const std::vector<DeviceReading> &devices, int primary_idx) {
  const DeviceReading *dev = resolve_device(ref, devices, primary_idx);
  if (!dev)
    return false;
  switch (ref.kind) {
  case SourceKind::none:
    return false;
  case SourceKind::button:
    return ref.index < dev->buttons.size() && dev->buttons[ref.index];
  case SourceKind::hat: {
    if (ref.index >= dev->hats.size())
      return false;
    const std::uint8_t mask = dev->hats[ref.index];
    switch (ref.hat_dir) {
    case HatDirection::up:
      return (mask & 0x01u) != 0;
    case HatDirection::right:
      return (mask & 0x02u) != 0;
    case HatDirection::down:
      return (mask & 0x04u) != 0;
    case HatDirection::left:
      return (mask & 0x08u) != 0;
    }
    return false;
  }
  case SourceKind::axis_positive:
    return ref.index < dev->axes.size() && dev->axes[ref.index] > 16384;
  case SourceKind::axis_negative:
    return ref.index < dev->axes.size() && dev->axes[ref.index] < -16384;
  case SourceKind::axis:
    return ref.index < dev->axes.size() && (dev->axes[ref.index] > 0 || std::abs(dev->axes[ref.index]) > 20000);
  }
  return false;
}

std::uint8_t evaluate_pedal_channel(const PedalConfig &pedal, const std::vector<DeviceReading> &devices, int primary_idx) {
  if (pedal.source.empty())
    return 0;
  const DeviceReading *dev = resolve_device(pedal.source, devices, primary_idx);
  if (!dev)
    return 0;
  if (pedal.source.kind == SourceKind::axis) {
    if (pedal.source.index >= dev->axes.size())
      return 0;
    return read_pedal(pedal, dev->axes[pedal.source.index], false);
  }
  const bool pressed = evaluate_digital_source(pedal.source, devices, primary_idx);
  return read_pedal(pedal, 0, pressed);
}

} // namespace

const char *pedal_axis_mode_name(PedalAxisMode mode) {
  switch (mode) {
  case PedalAxisMode::inverted:
    return "inverted";
  case PedalAxisMode::normal:
    return "normal";
  case PedalAxisMode::positive:
    return "positive";
  case PedalAxisMode::combined_negative:
    return "combined_negative";
  case PedalAxisMode::combined_positive:
    return "combined_positive";
  }
  return "inverted";
}

PedalAxisMode parse_pedal_axis_mode(std::string_view text, const std::string &source_name, std::size_t line_no) {
  if (text == "inverted")
    return PedalAxisMode::inverted;
  if (text == "normal")
    return PedalAxisMode::normal;
  if (text == "positive")
    return PedalAxisMode::positive;
  if (text == "combined_negative")
    return PedalAxisMode::combined_negative;
  if (text == "combined_positive")
    return PedalAxisMode::combined_positive;
  fail(source_name, line_no,
       "unknown pedal axis mode '" + std::string(text) +
           "' (expected inverted, normal, positive, combined_negative, or combined_positive)");
}

const char *curve_name(Curve curve) {
  switch (curve) {
  case Curve::linear:
    return "linear";
  case Curve::quadratic:
    return "quadratic";
  case Curve::cubic:
    return "cubic";
  case Curve::s_curve:
    return "s_curve";
  case Curve::smooth_power:
    return "smooth_power";
  case Curve::sublinear:
    return "sublinear";
  case Curve::logarithmic:
    return "logarithmic";
  }
  return "linear";
}

SourceRef parse_source_ref(std::string_view text, const std::string &source_name, std::size_t line_no) {
  text = trim(text);
  if (text.empty() || text == "none")
    return {};

  SourceRef ref;
  if (text.starts_with("dev:")) {
    text.remove_prefix(4);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
      fail(source_name, line_no, "malformed device source (expected dev:<32-hex-guid>:<source>)");
    const std::string_view guid = text.substr(0, colon);
    if (!is_valid_guid_string(guid))
      fail(source_name, line_no, "invalid device GUID '" + std::string(guid) + "' in source");
    ref.device_guid = to_lower_ascii(guid);
    text.remove_prefix(colon + 1);
  }

  if (text.starts_with("axis:")) {
    std::string_view rest = text.substr(5);
    if (rest.ends_with('+')) {
      ref.kind = SourceKind::axis_positive;
      rest.remove_suffix(1);
    } else if (rest.ends_with('-')) {
      ref.kind = SourceKind::axis_negative;
      rest.remove_suffix(1);
    } else {
      ref.kind = SourceKind::axis;
    }
    ref.index = parse_u8_index(rest, 31, source_name, line_no, "axis");
    return ref;
  }

  if (text.starts_with("btn:")) {
    ref.kind = SourceKind::button;
    ref.index = parse_u8_index(text.substr(4), 127, source_name, line_no, "button");
    return ref;
  }

  if (text.starts_with("hat:")) {
    std::string_view rest = text.substr(4);
    const std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos)
      fail(source_name, line_no, "malformed hat source (expected hat:<index>:up|right|down|left)");
    ref.kind = SourceKind::hat;
    ref.index = parse_u8_index(rest.substr(0, colon), 7, source_name, line_no, "hat");
    const std::string_view dir = rest.substr(colon + 1);
    if (dir == "up")
      ref.hat_dir = HatDirection::up;
    else if (dir == "right")
      ref.hat_dir = HatDirection::right;
    else if (dir == "down")
      ref.hat_dir = HatDirection::down;
    else if (dir == "left")
      ref.hat_dir = HatDirection::left;
    else
      fail(source_name, line_no, "unknown hat direction '" + std::string(dir) + "'");
    return ref;
  }

  fail(source_name, line_no, "unknown wheel source '" + std::string(text) + "'");
}

std::string format_source_ref(const SourceRef &ref) {
  if (ref.empty())
    return "";
  std::string prefix;
  if (!ref.device_guid.empty())
    prefix = "dev:" + to_lower_ascii(ref.device_guid) + ":";
  switch (ref.kind) {
  case SourceKind::none:
    return "";
  case SourceKind::axis:
    return prefix + "axis:" + std::to_string(ref.index);
  case SourceKind::axis_positive:
    return prefix + "axis:" + std::to_string(ref.index) + "+";
  case SourceKind::axis_negative:
    return prefix + "axis:" + std::to_string(ref.index) + "-";
  case SourceKind::button:
    return prefix + "btn:" + std::to_string(ref.index);
  case SourceKind::hat: {
    const char *dir = "up";
    switch (ref.hat_dir) {
    case HatDirection::up:
      dir = "up";
      break;
    case HatDirection::right:
      dir = "right";
      break;
    case HatDirection::down:
      dir = "down";
      break;
    case HatDirection::left:
      dir = "left";
      break;
    }
    return prefix + "hat:" + std::to_string(ref.index) + ":" + dir;
  }
  }
  return "";
}

const WheelConfig &default_wheel_config() {
  static const WheelConfig kDefault = [] {
    WheelConfig config;
    config.enabled = false;
    config.preset = "generic";
    config.steering.source = {SourceKind::axis, {}, 0, HatDirection::up};
    config.throttle.source = {SourceKind::axis, {}, 1, HatDirection::up};
    config.brake.source = {SourceKind::axis, {}, 2, HatDirection::up};
    config.clutch.source = {SourceKind::axis, {}, 3, HatDirection::up};
    config.handbrake.deadzone = config.clutch.deadzone = 0.05;
    config.handbrake.saturation = config.clutch.saturation = 0.95;
    assign_default_buttons(config);
    return config;
  }();
  return kDefault;
}

WheelConfig parse_wheel_config(std::string_view text, const std::string &source_name) {
  WheelConfig config = default_wheel_config();
  std::string current_section;
  bool seen_version = false;

  auto parse_pedal_kv = [&](PedalConfig &pedal, std::string_view key, std::string_view value, std::size_t line_no) {
    if (key == "source") {
      const std::string_view src_str = unquote(value, source_name, line_no);
      pedal.source = parse_source_ref(src_str, source_name, line_no);
    } else if (key == "mode") {
      pedal.mode = parse_pedal_axis_mode(unquote(value, source_name, line_no), source_name, line_no);
    } else if (key == "deadzone") {
      const double dz = parse_double_token(value, source_name, line_no, "deadzone");
      if (dz < 0.0 || dz > 0.45)
        fail(source_name, line_no, "pedal deadzone must be between 0.0 and 0.45");
      pedal.deadzone = dz;
      if (pedal.saturation <= pedal.deadzone + 0.05)
        fail(source_name, line_no, "pedal saturation must exceed deadzone by at least 0.05");
    } else if (key == "saturation") {
      const double sat = parse_double_token(value, source_name, line_no, "saturation");
      if (sat < 0.20 || sat > 1.0)
        fail(source_name, line_no, "pedal saturation must be between 0.20 and 1.0");
      pedal.saturation = sat;
      if (pedal.saturation <= pedal.deadzone + 0.05)
        fail(source_name, line_no, "pedal saturation must exceed deadzone by at least 0.05");
    } else if (key == "curve") {
      pedal.curve = parse_curve_token(unquote(value, source_name, line_no), source_name, line_no);
    } else {
      fail(source_name, line_no, "unknown pedal key '" + std::string(key) + "'");
    }
  };

  std::size_t line_no = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const std::string_view raw_line = nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
    ++line_no;
    std::string_view line = trim(strip_comment(raw_line, source_name, line_no));
    if (!line.empty()) {
      if (line.front() == '[') {
        if (line.back() != ']')
          fail(source_name, line_no, "malformed table header");
        current_section = std::string(trim(line.substr(1, line.size() - 2)));
        if (current_section != "wheel" && current_section != "steering" && current_section != "throttle" &&
            current_section != "brake" && current_section != "handbrake" && current_section != "clutch" &&
            current_section != "ffb" && current_section != "buttons") {
          fail(source_name, line_no, "unknown section '[" + current_section + "]'");
        }
      } else {
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
          fail(source_name, line_no, "expected key = value");
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key.empty() || value.empty())
          fail(source_name, line_no, "expected key = value");

        if (current_section.empty() || current_section == "wheel") {
          if (key == "version" || key == "schema") {
            const int ver = parse_int_token(value, source_name, line_no, "version");
            if (ver != 1)
              fail(source_name, line_no, "unsupported wheel config version (expected 1)");
            config.version = 1;
            seen_version = true;
          } else if (key == "enabled") {
            config.enabled = parse_bool_token(value, source_name, line_no);
          } else if (key == "device_guid") {
            const std::string_view guid = unquote(value, source_name, line_no);
            if (!guid.empty() && !is_valid_guid_string(guid))
              fail(source_name, line_no, "device_guid must be empty or a 32-character hex GUID");
            config.device_guid = to_lower_ascii(guid);
          } else if (key == "device_name") {
            config.device_name = std::string(unquote(value, source_name, line_no));
          } else if (key == "preset") {
            config.preset = std::string(unquote(value, source_name, line_no));
          } else if (key == "mode") {
            const std::string_view mode = unquote(value, source_name, line_no);
            if (mode == "pad")
              fail(source_name, line_no,
                   "mode \"pad\" (the 1.2.x bridge onto the DualShock 2) was removed; the wheel drives the virtual "
                   "Driving Force Pro (mode = \"dfp\")");
            if (mode != "dfp")
              fail(source_name, line_no, "mode must be \"dfp\"");
          } else if (key == "pedal_map_mode") {
            (void)unquote(value, source_name, line_no); // the removed pad bridge's pedal routing; ignored
          } else {
            fail(source_name, line_no, "unknown [wheel] key '" + std::string(key) + "'");
          }
        } else if (current_section == "steering") {
          if (key == "source") {
            const SourceRef ref = parse_source_ref(unquote(value, source_name, line_no), source_name, line_no);
            if (!ref.empty() && ref.kind != SourceKind::axis)
              fail(source_name, line_no, "steering source must be an axis (e.g. \"axis:0\")");
            config.steering.source = ref;
          } else if (key == "rotation_degrees") {
            const int deg = parse_int_token(value, source_name, line_no, "rotation_degrees");
            if (deg < 180 || deg > 1080)
              fail(source_name, line_no, "rotation_degrees must be between 180 and 1080");
            config.steering.rotation_degrees = deg;
          } else if (key == "invert") {
            config.steering.invert = parse_bool_token(value, source_name, line_no);
          } else if (key == "effective_degrees" || key == "deadzone" || key == "linearity" || key == "sensitivity") {
            // Pad bridge shaping of the stick; the DFP gets the wheel's real angle. Ignored.
          } else {
            fail(source_name, line_no, "unknown [steering] key '" + std::string(key) + "'");
          }
        } else if (current_section == "throttle") {
          parse_pedal_kv(config.throttle, key, value, line_no);
        } else if (current_section == "brake") {
          parse_pedal_kv(config.brake, key, value, line_no);
        } else if (current_section == "handbrake") {
          parse_pedal_kv(config.handbrake, key, value, line_no);
        } else if (current_section == "clutch") {
          parse_pedal_kv(config.clutch, key, value, line_no);
        } else if (current_section == "ffb") {
          if (key == "enabled") {
            config.ffb.enabled = parse_bool_token(value, source_name, line_no);
          } else if (key == "strength") {
            const double s = parse_double_token(value, source_name, line_no, "strength");
            if (s < 0.0 || s > 2.0)
              fail(source_name, line_no, "ffb strength must be between 0.0 and 2.0");
            config.ffb.strength = s;
          } else if (key == "damper") {
            const double d = parse_double_token(value, source_name, line_no, "damper");
            if (d < 0.0 || d > 1.0)
              fail(source_name, line_no, "ffb damper must be between 0.0 and 1.0");
            config.ffb.damper = d;
          } else if (key == "invert") {
            config.ffb.invert = parse_bool_token(value, source_name, line_no);
          } else if (key == "centering_spring" || key == "curb_rumble") {
            // Pad bridge rumble shaping; the game's own forces replace it. Ignored.
          } else {
            fail(source_name, line_no, "unknown [ffb] key '" + std::string(key) + "'");
          }
        } else if (current_section == "buttons") {
          const auto slot = find_button_slot(key);
          if (!slot)
            fail(source_name, line_no, "unknown button input '" + std::string(key) + "'");
          const auto raw_items = parse_string_list(value, source_name, line_no);
          std::vector<SourceRef> parsed;
          for (std::string_view item : raw_items) {
            const SourceRef ref = parse_source_ref(item, source_name, line_no);
            if (ref.empty())
              continue;
            if (ref.kind == SourceKind::axis)
              fail(source_name, line_no,
                   "button '" + std::string(key) + "' requires btn:, hat:, or axis:N+/N- threshold source");
            if (std::find(parsed.begin(), parsed.end(), ref) != parsed.end())
              fail(source_name, line_no, "duplicate source '" + std::string(item) + "' on '" + std::string(key) + "'");
            parsed.push_back(ref);
          }
          config.buttons[*slot] = std::move(parsed);
        }
      }
    }
    if (nl == std::string_view::npos)
      break;
    pos = nl + 1;
  }

  if (!seen_version)
    fail(source_name, 1, "missing required version = 1");
  return config;
}

std::string serialize_wheel_config(const WheelConfig &config) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "# GT4Recomp racing wheel: the virtual Driving Force Pro (TASK-018, TASK-181)\n";
  out << "[wheel]\n";
  out << "version = " << config.version << "\n";
  out << "enabled = " << (config.enabled ? "true" : "false") << "\n";
  out << "mode = \"dfp\"\n";
  out << "device_guid = \"" << config.device_guid << "\"\n";
  out << "device_name = \"" << config.device_name << "\"\n";
  out << "preset = \"" << config.preset << "\"\n\n";

  out << "[steering]\n";
  out << "source = \"" << format_source_ref(config.steering.source) << "\"\n";
  out << "rotation_degrees = " << config.steering.rotation_degrees << "\n";
  out << "invert = " << (config.steering.invert ? "true" : "false") << "\n\n";

  auto write_pedal = [&](const char *section, const PedalConfig &pedal) {
    out << "[" << section << "]\n";
    out << "source = \"" << format_source_ref(pedal.source) << "\"\n";
    out << "mode = \"" << pedal_axis_mode_name(pedal.mode) << "\"\n";
    out << "deadzone = " << pedal.deadzone << "\n";
    out << "saturation = " << pedal.saturation << "\n";
    out << "curve = \"" << curve_name(pedal.curve) << "\"\n\n";
  };

  write_pedal("throttle", config.throttle);
  write_pedal("brake", config.brake);
  write_pedal("handbrake", config.handbrake);
  write_pedal("clutch", config.clutch);

  out << "[ffb]\n";
  out << "enabled = " << (config.ffb.enabled ? "true" : "false") << "\n";
  out << "strength = " << config.ffb.strength << "\n";
  out << "damper = " << config.ffb.damper << "\n";
  out << "invert = " << (config.ffb.invert ? "true" : "false") << "\n\n";

  out << "[buttons]\n";
  for (std::size_t i = 0; i < button_slots.size(); ++i) {
    out << button_slots[i].name << " = [";
    const auto &list = config.buttons[i];
    for (std::size_t j = 0; j < list.size(); ++j) {
      if (j > 0)
        out << ", ";
      out << "\"" << format_source_ref(list[j]) << "\"";
    }
    out << "]\n";
  }
  return out.str();
}

WheelConfig load_wheel_file(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw ConfigError(path + ":1: cannot open wheel configuration file");
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.eof() && file.fail())
    throw ConfigError(path + ":1: cannot read wheel configuration file");
  return parse_wheel_config(buffer.str(), path);
}

namespace {

std::string environment_value(const char *name) {
  char *value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    return {};
  std::string text(value);
  std::free(value);
  return text;
}

// GT4_WHEEL_FILE (the launcher sets it at Play and writes it in game.ini), else a wheel.toml beside the controls file
// (GT4_CONTROLS_FILE, <user>\config), else beside the game executable.
std::string find_wheel_file() {
  if (std::string path = environment_value("GT4_WHEEL_FILE"); !path.empty())
    return path;
  std::vector<std::filesystem::path> candidates;
  if (const std::string controls = environment_value("GT4_CONTROLS_FILE"); !controls.empty())
    candidates.push_back(std::filesystem::path(controls).parent_path() / "wheel.toml");
  wchar_t module[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0)
    candidates.push_back(std::filesystem::path(module).parent_path() / "wheel.toml");
  for (const auto &candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error))
      return candidate.string();
  }
  return {};
}

} // namespace

WheelFileResult load_wheel_from_environment() {
  WheelFileResult result;
  result.config = default_wheel_config();
  result.config.enabled = false;
  result.path = find_wheel_file();
  if (result.path.empty())
    return result;
  try {
    result.config = load_wheel_file(result.path);
  } catch (const ConfigError &error) {
    result.error = error.what();
    result.config = default_wheel_config();
    result.config.enabled = false;
  }
  return result;
}

double normalize_pedal_axis(PedalAxisMode mode, std::int16_t raw) {
  switch (mode) {
  case PedalAxisMode::inverted:
    // +32767 at rest -> 0.0, -32768 at full press -> 1.0
    return std::clamp((32767.0 - static_cast<double>(raw)) / 65535.0, 0.0, 1.0);
  case PedalAxisMode::normal:
    // -32768 at rest -> 0.0, +32767 at full press -> 1.0
    return std::clamp((static_cast<double>(raw) + 32768.0) / 65535.0, 0.0, 1.0);
  case PedalAxisMode::positive:
  case PedalAxisMode::combined_positive:
    // 0 at rest -> 0.0, +32767 at full press -> 1.0
    return raw <= 0 ? 0.0 : std::clamp(static_cast<double>(raw) / 32767.0, 0.0, 1.0);
  case PedalAxisMode::combined_negative:
    // 0 at rest -> 0.0, -32768 at full press -> 1.0
    return raw >= 0 ? 0.0 : std::clamp(static_cast<double>(-static_cast<int>(raw)) / 32768.0, 0.0, 1.0);
  }
  return 0.0;
}

std::uint8_t read_pedal(const PedalConfig &config, std::int16_t raw_axis, bool digital_pressed) {
  if (config.source.empty())
    return 0;
  if (config.source.kind != SourceKind::axis)
    return digital_pressed ? 255 : 0;

  const double travel = normalize_pedal_axis(config.mode, raw_axis);
  const double dz = std::clamp(config.deadzone, 0.0, 0.45);
  const double sat = std::clamp(config.saturation, std::min(1.0, dz + 0.05), 1.0);
  if (travel <= dz)
    return 0;
  const double normalized = std::clamp((travel - dz) / (sat - dz), 0.0, 1.0);
  const double curved = std::clamp(apply_curve(config.curve, normalized), 0.0, 1.0);
  const int byte_val = static_cast<int>(std::lround(curved * 255.0));
  return static_cast<std::uint8_t>(std::clamp(std::max(1, byte_val), 1, 255));
}

bool extract_guid_vid_pid(std::string_view guid, std::uint16_t &vid, std::uint16_t &pid) {
  if (guid.size() != 32 || !is_valid_guid_string(guid))
    return false;
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
  };
  auto parse_u16_le = [&](std::size_t offset) {
    const int low = hex_val(guid[offset]) * 16 + hex_val(guid[offset + 1]);
    const int high = hex_val(guid[offset + 2]) * 16 + hex_val(guid[offset + 3]);
    return static_cast<std::uint16_t>(low | (high << 8));
  };
  vid = parse_u16_le(8);
  pid = parse_u16_le(16);
  return true;
}

bool is_known_wheel_vid_pid(std::uint16_t vid, std::uint16_t pid) {
  struct Model {
    std::uint16_t vid, pid;
  };
  // Confirmed IDs of docs/research/WHEEL-AUTOMAP-TABLE.md (SDL's wheel list plus Linux compatibility tables).
  static constexpr Model kWheels[] = {
      // Logitech: Driving Force/EX, MOMO Force, DFP, G25, DFGT, G27, G29 (PS3, PS4), G920, G923, G PRO.
      {0x046D, 0xC294}, {0x046D, 0xC295}, {0x046D, 0xC298}, {0x046D, 0xC299}, {0x046D, 0xC29A}, {0x046D, 0xC29B},
      {0x046D, 0xC24F}, {0x046D, 0xC260}, {0x046D, 0xC261}, {0x046D, 0xC262}, {0x046D, 0xC266}, {0x046D, 0xC267},
      {0x046D, 0xC26D}, {0x046D, 0xC26E}, {0x046D, 0xC268}, {0x046D, 0xC269}, {0x046D, 0xC272},
      // Thrustmaster: T150, TMX, T248, T300RS (three modes), TX (initial, active), TS-XW (initial, active).
      {0x044F, 0xB677}, {0x044F, 0xB67F}, {0x044F, 0xB696}, {0x044F, 0xB66E}, {0x044F, 0xB66F}, {0x044F, 0xB66D},
      {0x044F, 0xB664}, {0x044F, 0xB669}, {0x044F, 0xB691}, {0x044F, 0xB692},
      // Fanatec: ClubSport V2, V2.5, CSL Elite (PC, PS4), Podium DD1, DD2, CSR, CSL DD / GT DD Pro / ClubSport DD.
      {0x0EB7, 0x0001}, {0x0EB7, 0x0004}, {0x0EB7, 0x0E03}, {0x0EB7, 0x0005}, {0x0EB7, 0x0006}, {0x0EB7, 0x0007},
      {0x0EB7, 0x0011}, {0x0EB7, 0x0020},
      // MOZA (vendor 0x346E): R16/R21, R9, R5, R3, R12.
      {0x346E, 0x0000}, {0x346E, 0x0002}, {0x346E, 0x0004}, {0x346E, 0x0005}, {0x346E, 0x0006},
      // Simagic: Alpha / Alpha Mini / Alpha Ultimate / M10 (one ID), EVO Sport, EVO, EVO Pro.
      {0x0483, 0x0522}, {0x3670, 0x0500}, {0x3670, 0x0501}, {0x3670, 0x0502},
      // Turtle Beach VelocityOne Race.
      {0x10F5, 0x7077},
      // PXN V10, V12, V12 Lite (two IDs), VD6.
      {0x11FF, 0x3245}, {0x11FF, 0x1212}, {0x11FF, 0x1112}, {0x11FF, 0x1211}, {0x36E6, 0x400F},
      // Cammus C5, C12.
      {0x3416, 0x0301}, {0x3416, 0x0302},
  };
  return std::any_of(std::begin(kWheels), std::end(kWheels),
                     [vid, pid](const Model &model) { return model.vid == vid && model.pid == pid; });
}

bool is_candidate_wheel_device(bool sdl_is_wheel_type, bool sdl_is_gamepad, std::string_view device_name,
                               std::string_view guid) {
  (void)sdl_is_gamepad; // XInput wheels (HORI) are gamepads too; the type alone decides nothing
  const std::string lower = to_lower_ascii(device_name);
  for (std::string_view auxiliary : {"pedal", "shifter", "shift", "handbrake"})
    if (lower.find(auxiliary) != std::string::npos)
      return false;
  if (sdl_is_wheel_type)
    return true;
  std::uint16_t vid = 0, pid = 0;
  if (extract_guid_vid_pid(guid, vid, pid) && is_known_wheel_vid_pid(vid, pid))
    return true;
  constexpr std::array<std::string_view, 25> wheel_names{{
      "wheel", "driving force", "g29", "g920", "g923", "g27", "g25", "t150", "t248", "t300", "t500", "t-gt", "t818",
      "tmx", "apex", "overdrive", "racing base", "wheelbase", "moza", "fanatec", "clubsport", "podium", "simagic",
      "cammus", "velocityone race"}};
  return std::any_of(wheel_names.begin(), wheel_names.end(),
                     [&lower](std::string_view name) { return lower.find(name) != std::string::npos; });
}

int select_primary_device(const WheelConfig &config, const std::vector<DeviceReading> &devices) {
  if (!config.enabled)
    return -1;
  int selected = -1;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const auto &device = devices[i];
    if (!config.device_guid.empty()) {
      if (to_lower_ascii(device.guid) == to_lower_ascii(config.device_guid)) {
        if (selected >= 0)
          return -1;
        selected = static_cast<int>(i);
      }
    } else if (is_candidate_wheel_device(device.is_wheel, device.is_gamepad, device.name, device.guid)) {
      return static_cast<int>(i);
    }
  }
  return selected;
}

gt4::usb::DfpInput map_dfp_input(const WheelConfig &config, const std::vector<DeviceReading> &devices, int primary,
                                 int game_range_degrees) {
  gt4::usb::DfpInput input; // neutral: centred, nothing pressed
  if (!config.enabled)
    return input;
  // Steering: the wheel's physical angle on the range the game set. A 900-degree wheel on GT4's 900 degrees is 1:1; on a
  // 200-degree range it reaches full lock at 100 degrees each way.
  if (config.steering.source.kind == SourceKind::axis) {
    const DeviceReading *device = resolve_device(config.steering.source, devices, primary);
    if (device && config.steering.source.index < device->axes.size()) {
      const std::int16_t raw = device->axes[config.steering.source.index];
      double position = raw < 0 ? raw / 32768.0 : raw / 32767.0;
      if (config.steering.invert)
        position = -position;
      const double degrees = position * std::clamp(config.steering.rotation_degrees, 180, 1080) / 2.0;
      const double half_range = std::max(game_range_degrees, 40) / 2.0;
      const double scaled = std::clamp(degrees / half_range, -1.0, 1.0);
      // 0 = full left, 0x1FFF centre, 0x3FFF full right.
      const long offset = std::lround(scaled * (scaled < 0.0 ? 0x1FFF : 0x2000));
      input.steering = static_cast<std::uint16_t>(std::clamp(0x1FFFL + offset, 0L, 0x3FFFL));
    }
  }
  input.throttle = evaluate_pedal_channel(config.throttle, devices, primary);
  input.brake = evaluate_pedal_channel(config.brake, devices, primary);
  bool hat_up = false, hat_right = false, hat_down = false, hat_left = false;
  for (std::size_t slot = 0; slot < button_slots.size(); ++slot) {
    bool pressed = false;
    for (const SourceRef &source : config.buttons[slot])
      pressed = pressed || evaluate_digital_source(source, devices, primary);
    if (!pressed)
      continue;
    const ButtonSlotInfo &info = button_slots[slot];
    if (info.dfp_bit >= 0) {
      input.buttons |= static_cast<std::uint16_t>(1u << info.dfp_bit);
      continue;
    }
    const std::string_view name = info.name;
    hat_up = hat_up || name == "dpad_up";
    hat_right = hat_right || name == "dpad_right";
    hat_down = hat_down || name == "dpad_down";
    hat_left = hat_left || name == "dpad_left";
  }
  if (evaluate_pedal_channel(config.handbrake, devices, primary) > 128)
    input.buttons |= 1u << 2; // the handbrake pedal presses Circle
  if (hat_up && hat_right)
    input.hat = 1;
  else if (hat_right && hat_down)
    input.hat = 3;
  else if (hat_down && hat_left)
    input.hat = 5;
  else if (hat_left && hat_up)
    input.hat = 7;
  else if (hat_up)
    input.hat = 0;
  else if (hat_right)
    input.hat = 2;
  else if (hat_down)
    input.hat = 4;
  else if (hat_left)
    input.hat = 6;
  return input;
}

// ------------------------------------------------------------------------------------------------- SDL3 haptic sink

namespace {

Uint16 sdl_effect_type(EffectKind kind) {
  switch (kind) {
  case EffectKind::constant:
    return SDL_HAPTIC_CONSTANT;
  case EffectKind::spring:
    return SDL_HAPTIC_SPRING;
  case EffectKind::damper:
    return SDL_HAPTIC_DAMPER;
  case EffectKind::friction:
    return SDL_HAPTIC_FRICTION;
  }
  return SDL_HAPTIC_CONSTANT;
}

SDL_HapticEffect sdl_effect(EffectKind kind, const EffectParams &params) {
  SDL_HapticEffect effect{};
  const Uint16 type = sdl_effect_type(kind);
  effect.type = type;
  if (kind == EffectKind::constant) {
    effect.constant.type = type;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = 1;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = params.level;
  } else {
    effect.condition.type = type;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.left_coeff[0] = params.left_coeff;
    effect.condition.right_coeff[0] = params.right_coeff;
    effect.condition.left_sat[0] = params.left_sat;
    effect.condition.right_sat[0] = params.right_sat;
    effect.condition.center[0] = params.center;
    effect.condition.deadband[0] = params.deadband;
  }
  return effect;
}

// One SDL haptic handle on the wheel; every force goes through it. The withdrawn 1.2.x bridge also called
// SDL_RumbleJoystick, a second effect owner on a DirectInput wheel, and the MOZA R5 crashed the game in pid.dll with both
// active (2026-11-05). Features the device lacks are never called (the R5 has no autocentre).
class SdlHapticSink final : public HapticSink {
public:
  SdlHapticSink(SDL_Haptic *haptic, Uint32 features) : haptic_(haptic), features_(features) {}
  ~SdlHapticSink() override { release(); }

  bool create(EffectKind kind, const EffectParams &params) override {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (!haptic_ || !(features_ & sdl_effect_type(kind)))
      return false;
    SDL_HapticEffect effect = sdl_effect(kind, params);
    ids_[index] = SDL_CreateHapticEffect(haptic_, &effect);
    if (ids_[index] < 0)
      std::fprintf(stderr, "[wheel] DFP %s effect not created: %s\n", effect_kind_name(kind), SDL_GetError());
    return ids_[index] >= 0;
  }
  bool update(EffectKind kind, const EffectParams &params) override {
    const int id = ids_[static_cast<std::size_t>(kind)];
    if (!haptic_ || id < 0)
      return false;
    SDL_HapticEffect effect = sdl_effect(kind, params);
    if (SDL_UpdateHapticEffect(haptic_, id, &effect))
      return true;
    if (errors_++ < 20)
      std::fprintf(stderr, "[wheel] DFP update of the %s effect failed: %s\n", effect_kind_name(kind), SDL_GetError());
    return false;
  }
  bool run(EffectKind kind) override {
    const int id = ids_[static_cast<std::size_t>(kind)];
    if (!haptic_ || id < 0)
      return false;
    if (SDL_RunHapticEffect(haptic_, id, SDL_HAPTIC_INFINITY))
      return true;
    std::fprintf(stderr, "[wheel] DFP %s effect not started: %s\n", effect_kind_name(kind), SDL_GetError());
    return false;
  }
  void stop_all() override {
    if (haptic_)
      SDL_StopHapticEffects(haptic_);
  }
  void set_gain(int percent) override {
    if (haptic_ && (features_ & SDL_HAPTIC_GAIN))
      SDL_SetHapticGain(haptic_, percent);
  }
  void release() override {
    if (!haptic_)
      return;
    SDL_StopHapticEffects(haptic_);
    for (int &id : ids_) {
      if (id >= 0)
        SDL_DestroyHapticEffect(haptic_, id);
      id = -1;
    }
    // With no effect left the gain is safe to give back at full, so the next program finds the wheel as it was.
    if (features_ & SDL_HAPTIC_GAIN)
      SDL_SetHapticGain(haptic_, 100);
    SDL_CloseHaptic(haptic_);
    haptic_ = nullptr;
  }

private:
  SDL_Haptic *haptic_;
  Uint32 features_;
  std::array<int, effect_kind_count> ids_{-1, -1, -1, -1};
  int errors_ = 0;
};

std::unique_ptr<HapticSink> open_sdl_haptics(void *device) {
  SDL_Joystick *joystick = static_cast<SDL_Joystick *>(device);
  SDL_Haptic *haptic = joystick ? SDL_OpenHapticFromJoystick(joystick) : nullptr;
  if (!haptic) {
    std::fprintf(stderr, "[wheel] the wheel has no force feedback through SDL haptics: %s\n", SDL_GetError());
    return nullptr;
  }
  const Uint32 features = SDL_GetHapticFeatures(haptic);
  // Opening never enables force: the gain stays 0 until the first force starts the effects, and the device's own
  // autocentre is off, because the game's springs (and the DFP's default spring) are played as effects.
  if (features & SDL_HAPTIC_GAIN)
    SDL_SetHapticGain(haptic, 0);
  if (features & SDL_HAPTIC_AUTOCENTER)
    SDL_SetHapticAutocenter(haptic, 0);
  std::fprintf(stderr, "[wheel] force feedback device: constant=%s spring=%s damper=%s friction=%s gain=%s autocenter=%s\n",
               (features & SDL_HAPTIC_CONSTANT) ? "yes" : "no", (features & SDL_HAPTIC_SPRING) ? "yes" : "no",
               (features & SDL_HAPTIC_DAMPER) ? "yes" : "no", (features & SDL_HAPTIC_FRICTION) ? "yes" : "no",
               (features & SDL_HAPTIC_GAIN) ? "yes" : "no", (features & SDL_HAPTIC_AUTOCENTER) ? "yes" : "no");
  return std::make_unique<SdlHapticSink>(haptic, features);
}

// The haptics of the running game, for the exit hook: std::exit() skips the local WheelInput's destructor.
std::atomic<DfpHapticThread *> g_exit_haptics{nullptr};

void stop_forces_at_exit() {
  if (DfpHapticThread *haptics = g_exit_haptics.exchange(nullptr)) {
    haptics->set_active(false);
    haptics->detach();
  }
}

} // namespace

// ------------------------------------------------------------------------------------------------------- WheelInput

bool WheelInput::haptic_thread_wanted() {
  return environment_value("GT4_WHEEL_HAPTIC_THREAD") != "0";
}

WheelInput::WheelInput(WheelConfig config, HapticSinkFactory haptics, bool threaded)
    : config_(std::move(config)), haptics_(config_.ffb, haptics ? std::move(haptics) : HapticSinkFactory(open_sdl_haptics),
                                           threaded && config_.enabled) {
  if (!config_.enabled)
    return;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK))
    sdl_initialized_ = true;
  else
    std::fprintf(stderr, "[wheel] SDL_InitSubSystem(SDL_INIT_JOYSTICK) failed: %s\n", SDL_GetError());
  if (config_.ffb.enabled && SDL_InitSubSystem(SDL_INIT_HAPTIC))
    haptic_initialized_ = true;
  // The USB capsule plugs the virtual Driving Force Pro in when the IOP's USB driver first touches the controller.
  gt4::usb::DfpBridge &bridge = gt4::usb::DfpBridge::shared();
  bridge.set_enabled(true);
  // Every force command of the game (on the emulation thread) hands the haptics owner the latest force set at once,
  // instead of waiting for the next VBlank: up to 16 ms less delay in the game's force loop.
  bridge.set_force_listener([this] { haptics_.set_forces(gt4::usb::DfpBridge::shared().forces()); });
  g_exit_haptics.store(&haptics_);
  static const bool exit_hook = [] { return std::atexit(stop_forces_at_exit) == 0; }();
  (void)exit_hook;
  std::fprintf(stderr, "[wheel] the wheel drives a virtual Driving Force Pro on the emulated USB bus (haptics %s)\n",
               haptics_.threaded() ? "on their own thread" : "on the emulation thread");
}

WheelInput::~WheelInput() {
  if (config_.enabled)
    gt4::usb::DfpBridge::shared().set_force_listener({});
  DfpHapticThread *self = &haptics_;
  g_exit_haptics.compare_exchange_strong(self, nullptr);
  haptics_.set_active(false);
  haptics_.detach(); // before the joysticks close: the haptic handle uses the wheel's DirectInput device
  if (config_.enabled)
    std::fprintf(stderr, "%s\n", force_summary().c_str());
  close_all_devices();
  if (haptic_initialized_)
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
  if (sdl_initialized_)
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

std::string WheelInput::force_summary() const {
  const DfpHapticThread::Stats stats = haptics_.stats();
  std::ostringstream out;
  out << "[wheel] force feedback summary: game force commands " << stats.force_sets << ", effect updates sent "
      << stats.updates_sent << ", effects started " << stats.starts << ", haptic device "
      << (stats.device_open ? "open" : "none") << ", effects " << (stats.effects_supported ? "supported" : "none")
      << ", game forces played " << stats.forces_played << ", reached the wheel "
      << (stats.forces_played > 0 ? "yes" : "no");
  return out.str();
}

void WheelInput::set_primary(SDL_Joystick *joystick, bool focused) {
  if (joystick == primary_joystick_)
    return;
  // Close the old haptic handle before the joystick changes; never send old effects to a new device.
  if (haptic_attached_) {
    haptics_.detach();
    haptic_attached_ = false;
  }
  primary_joystick_ = joystick;
  (void)focused;
  if (primary_joystick_ && haptic_initialized_ && config_.ffb.enabled) {
    haptics_.attach(primary_joystick_);
    haptic_attached_ = true;
  }
}

void WheelInput::close_all_devices() {
  set_primary(nullptr, false);
  for (auto &dev : devices_)
    if (dev.joystick)
      SDL_CloseJoystick(dev.joystick);
  devices_.clear();
}

bool WheelInput::references_device(const std::string &guid) const {
  const auto match = [&guid](const SourceRef &source) {
    return !source.device_guid.empty() && to_lower_ascii(source.device_guid) == to_lower_ascii(guid);
  };
  if (match(config_.steering.source) || match(config_.throttle.source) || match(config_.brake.source) ||
      match(config_.handbrake.source) || match(config_.clutch.source))
    return true;
  for (const auto &slot : config_.buttons)
    for (const auto &source : slot)
      if (match(source))
        return true;
  return false;
}

void WheelInput::scan_devices() {
  if (!sdl_initialized_)
    return;

  // Remove any disconnected joysticks first; the primary's haptic handle closes before its joystick.
  for (std::size_t i = 0; i < devices_.size();) {
    if (!devices_[i].joystick || !SDL_JoystickConnected(devices_[i].joystick)) {
      if (devices_[i].joystick == primary_joystick_)
        set_primary(nullptr, false);
      if (devices_[i].joystick)
        SDL_CloseJoystick(devices_[i].joystick);
      devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }

  int count = 0;
  SDL_JoystickID *ids = SDL_GetJoysticks(&count);
  if (!ids)
    return;

  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID id = ids[i];
    const bool already_open =
        std::any_of(devices_.begin(), devices_.end(), [id](const OpenDevice &d) { return d.instance_id == id; });
    if (already_open)
      continue;

    // Identify candidates without opening unrelated hardware handles.
    char candidate_guid[64]{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), candidate_guid, sizeof(candidate_guid));
    const std::string candidate = to_lower_ascii(candidate_guid);
    const char *candidate_name = SDL_GetJoystickNameForID(id);
    const bool referenced =
        (!config_.device_guid.empty() && to_lower_ascii(config_.device_guid) == candidate) || references_device(candidate);
    if (!referenced && (!config_.device_guid.empty() ||
                        !is_candidate_wheel_device(SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL,
                                                   SDL_IsGamepad(id), candidate_name ? candidate_name : "", candidate)))
      continue;
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    if (!joy)
      continue;

    char guid_buf[64]{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid_buf, sizeof(guid_buf));
    const char *raw_name = SDL_GetJoystickName(joy);
    OpenDevice entry;
    entry.instance_id = id;
    entry.joystick = joy;
    entry.guid = to_lower_ascii(guid_buf);
    entry.name = raw_name ? raw_name : "Unknown USB Device";
    entry.is_gamepad = SDL_IsGamepad(id);
    entry.is_wheel = is_candidate_wheel_device(SDL_GetJoystickType(joy) == SDL_JOYSTICK_TYPE_WHEEL, entry.is_gamepad,
                                               entry.name, entry.guid);
    std::uint16_t vid = 0, pid = 0;
    const bool has_ids = extract_guid_vid_pid(entry.guid, vid, pid);
    std::fprintf(stderr, "[wheel] device: %s (%04x:%04x, SDL type %s, %d axes, %d buttons, %d hats)\n",
                 entry.name.c_str(), has_ids ? vid : 0, has_ids ? pid : 0, entry.is_wheel ? "wheel" : "other",
                 SDL_GetNumJoystickAxes(joy), SDL_GetNumJoystickButtons(joy), SDL_GetNumJoystickHats(joy));
    devices_.push_back(std::move(entry));
  }
  SDL_free(ids);
}

std::uint32_t WheelInput::primary_instance_id() const {
  return primary_joystick_ ? SDL_GetJoystickID(primary_joystick_) : 0;
}

void WheelInput::prepare(bool focused) {
  owned_device_ids_.clear();
  if (!config_.enabled || !sdl_initialized_)
    return;
  SDL_UpdateJoysticks();
  const std::uint64_t now_ms = SDL_GetTicks();
  if (now_ms >= next_scan_ms_ || devices_.empty()) {
    next_scan_ms_ = now_ms + 2000;
    scan_devices();
  }
  std::vector<DeviceReading> readings;
  for (const auto &device : devices_) {
    if (!device.joystick || !SDL_JoystickConnected(device.joystick))
      continue;
    DeviceReading reading;
    reading.instance_id = device.instance_id;
    reading.guid = device.guid;
    reading.name = device.name;
    reading.is_wheel = device.is_wheel;
    reading.is_gamepad = device.is_gamepad;
    for (int i = 0; i < SDL_GetNumJoystickAxes(device.joystick); ++i)
      reading.axes.push_back(SDL_GetJoystickAxis(device.joystick, i));
    for (int i = 0; i < SDL_GetNumJoystickButtons(device.joystick); ++i)
      reading.buttons.push_back(SDL_GetJoystickButton(device.joystick, i));
    for (int i = 0; i < SDL_GetNumJoystickHats(device.joystick); ++i)
      reading.hats.push_back(SDL_GetJoystickHat(device.joystick, i));
    readings.push_back(std::move(reading));
  }
  const int selected = select_primary_device(config_, readings);
  const std::uint32_t selected_id = selected < 0 ? 0 : readings[static_cast<std::size_t>(selected)].instance_id;
  SDL_Joystick *resolved = nullptr;
  for (const auto &device : devices_)
    if (device.instance_id == selected_id)
      resolved = device.joystick;
  set_primary(resolved, focused);
  // Ownership: the gamepad path never reads the wheel or a device bound by GUID (when that GUID is unique).
  for (const auto &device : readings)
    if (device.instance_id == selected_id ||
        (references_device(device.guid) &&
         std::count_if(readings.begin(), readings.end(),
                       [&device](const DeviceReading &candidate) { return candidate.guid == device.guid; }) == 1))
      owned_device_ids_.push_back(device.instance_id);

  gt4::usb::DfpBridge &bridge = gt4::usb::DfpBridge::shared();
  bridge.set_input(focused ? map_dfp_input(config_, readings, selected, bridge.forces().range_degrees)
                           : gt4::usb::DfpInput{});
  haptics_.set_active(focused);
}

void WheelInput::stop() {
  if (config_.enabled)
    gt4::usb::DfpBridge::shared().set_input({});
  haptics_.set_active(false);
}

} // namespace gt4::wheel
