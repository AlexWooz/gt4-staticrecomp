#include "wheel.hpp"

#include "usb_dfp.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
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

double apply_curve(Curve curve, double value) {
  switch (curve) {
  case Curve::linear:
    return value;
  case Curve::quadratic:
    return value * value;
  case Curve::cubic:
    return value * value * value;
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

std::optional<std::size_t> find_button_slot(std::string_view key) {
  for (std::size_t i = 0; i < button_slots.size(); ++i) {
    if (key == button_slots[i].name)
      return i;
  }
  // Friendly racing wheel aliases accepted in wheel.toml [buttons]:
  if (key == "shift_up" || key == "gear_up" || key == "paddle_right")
    return 11; // r2
  if (key == "shift_down" || key == "gear_down" || key == "paddle_left")
    return 10; // l2
  if (key == "look_back" || key == "rear_view")
    return 8; // l1
  if (key == "nitro" || key == "boost" || key == "high_beam")
    return 9; // r1
  if (key == "reverse")
    return 7; // triangle
  if (key == "handbrake_btn" || key == "ebrake")
    return 5; // circle
  if (key == "camera" || key == "view" || key == "change_view")
    return 14; // select
  if (key == "pause")
    return 15; // start
  if (key == "horn")
    return 12; // l3
  return std::nullopt;
}

void assign_default_buttons(WheelConfig &cfg) {
  for (auto &slot : cfg.buttons)
    slot.clear();
  cfg.buttons[0] = {{SourceKind::hat, {}, 0, HatDirection::up}};    // dpad_up
  cfg.buttons[1] = {{SourceKind::hat, {}, 0, HatDirection::right}}; // dpad_right
  cfg.buttons[2] = {{SourceKind::hat, {}, 0, HatDirection::down}};  // dpad_down
  cfg.buttons[3] = {{SourceKind::hat, {}, 0, HatDirection::left}};  // dpad_left
  cfg.buttons[4] = {{SourceKind::button, {}, 0, HatDirection::up}}; // cross
  cfg.buttons[5] = {{SourceKind::button, {}, 2, HatDirection::up}}; // circle
  cfg.buttons[6] = {{SourceKind::button, {}, 1, HatDirection::up}}; // square
  cfg.buttons[7] = {{SourceKind::button, {}, 3, HatDirection::up}}; // triangle
  cfg.buttons[8] = {{SourceKind::button, {}, 7, HatDirection::up}}; // l1 (look back)
  cfg.buttons[9] = {{SourceKind::button, {}, 6, HatDirection::up}}; // r1 (nitro)
  cfg.buttons[10] = {{SourceKind::button, {}, 5, HatDirection::up}}; // l2 (shift down)
  cfg.buttons[11] = {{SourceKind::button, {}, 4, HatDirection::up}}; // r2 (shift up)
  cfg.buttons[12] = {{SourceKind::button, {}, 10, HatDirection::up}}; // l3
  cfg.buttons[13] = {{SourceKind::button, {}, 11, HatDirection::up}}; // r3
  cfg.buttons[14] = {{SourceKind::button, {}, 8, HatDirection::up}};  // select (camera)
  cfg.buttons[15] = {{SourceKind::button, {}, 9, HatDirection::up}};  // start (pause)
}

void press_pad_button(PadState &pad, std::uint8_t bit, std::uint8_t pressure) {
  if (pressure == 0)
    return;
  pad.buttons |= static_cast<std::uint16_t>(1u << bit);
  pad.pressures[bit] = std::max(pad.pressures[bit], pressure);
}

const DeviceReading *resolve_device(const SourceRef &ref, const std::vector<DeviceReading> &devices, int primary_idx) {
  if (ref.empty())
    return nullptr;
  if (!ref.device_guid.empty()) {
    const std::string target = to_lower_ascii(ref.device_guid);
    const DeviceReading *match = nullptr;
    for (const auto &device : devices) {
      if (to_lower_ascii(device.guid) == target) {
        if (match) return nullptr; // Identical model GUIDs cannot identify a unique physical device.
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

const char *pedal_map_mode_name(PedalMapMode mode) {
  switch (mode) {
  case PedalMapMode::cross_square:
    return "cross_square";
  case PedalMapMode::rstick_y:
    return "rstick_y";
  case PedalMapMode::l2_r2:
    return "l2_r2";
  case PedalMapMode::both:
    return "both";
  }
  return "cross_square";
}

PedalMapMode parse_pedal_map_mode(std::string_view text, const std::string &source_name, std::size_t line_no) {
  if (text == "cross_square")
    return PedalMapMode::cross_square;
  if (text == "rstick_y")
    return PedalMapMode::rstick_y;
  if (text == "l2_r2")
    return PedalMapMode::l2_r2;
  if (text == "both")
    return PedalMapMode::both;
  fail(source_name, line_no,
       "unknown pedal_map_mode '" + std::string(text) + "' (expected cross_square, rstick_y, l2_r2, or both)");
}

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

WheelConfig preset_wheel_config(std::string_view preset_name) {
  WheelConfig cfg;
  cfg.version = 1;
  cfg.enabled = true;
  cfg.preset = std::string(preset_name);
  cfg.pedal_map_mode = PedalMapMode::cross_square;

  cfg.steering.source = {SourceKind::axis, {}, 0, HatDirection::up};
  cfg.steering.rotation_degrees = 900;
  cfg.steering.effective_degrees = 900;
  cfg.steering.deadzone = 0.0;
  cfg.steering.linearity = Curve::linear;
  cfg.steering.sensitivity = 1.0;
  cfg.steering.invert = false;

  cfg.throttle.source = {SourceKind::axis, {}, 1, HatDirection::up};
  cfg.throttle.mode = PedalAxisMode::inverted;
  cfg.throttle.deadzone = 0.02;
  cfg.throttle.saturation = 0.98;
  cfg.throttle.curve = Curve::linear;

  cfg.brake.source = {SourceKind::axis, {}, 2, HatDirection::up};
  cfg.brake.mode = PedalAxisMode::inverted;
  cfg.brake.deadzone = 0.02;
  cfg.brake.saturation = 0.98;
  cfg.brake.curve = Curve::linear;

  cfg.handbrake.source = {};
  cfg.handbrake.mode = PedalAxisMode::inverted;
  cfg.handbrake.deadzone = 0.05;
  cfg.handbrake.saturation = 0.95;
  cfg.handbrake.curve = Curve::linear;

  cfg.clutch.source = {SourceKind::axis, {}, 3, HatDirection::up};
  cfg.clutch.mode = PedalAxisMode::inverted;
  cfg.clutch.deadzone = 0.05;
  cfg.clutch.saturation = 0.95;
  cfg.clutch.curve = Curve::linear;

  cfg.ffb.enabled = true;
  cfg.ffb.strength = 1.0;
  cfg.ffb.centering_spring = 0.25;
  cfg.ffb.damper = 0.15;
  cfg.ffb.curb_rumble = 1.0;
  cfg.ffb.invert = false;

  assign_default_buttons(cfg);

  if (preset_name == "generic" || preset_name == "logitech_g29" || preset_name.empty()) {
    if (preset_name.empty())
      cfg.preset = "generic";
    return cfg;
  }

  if (preset_name == "hori") {
    // HORI Racing Wheel Apex / Overdrive / XInput (270 deg):
    // Throttle is RT (axis 5, normal mode: -32768 rest to +32767 full)
    // Brake is LT (axis 2, normal mode: -32768 rest to +32767 full)
    // No clutch pedal.
    cfg.steering.rotation_degrees = 270;
    cfg.steering.effective_degrees = 270;
    cfg.throttle.source = {SourceKind::axis, {}, 5, HatDirection::up};
    cfg.throttle.mode = PedalAxisMode::normal;
    cfg.brake.source = {SourceKind::axis, {}, 2, HatDirection::up};
    cfg.brake.mode = PedalAxisMode::normal;
    cfg.clutch.source = {};
    cfg.handbrake.source = {};
    cfg.buttons[0] = {{SourceKind::hat, {}, 0, HatDirection::up}};    // dpad_up
    cfg.buttons[1] = {{SourceKind::hat, {}, 0, HatDirection::right}}; // dpad_right
    cfg.buttons[2] = {{SourceKind::hat, {}, 0, HatDirection::down}};  // dpad_down
    cfg.buttons[3] = {{SourceKind::hat, {}, 0, HatDirection::left}};  // dpad_left
    cfg.buttons[4] = {{SourceKind::button, {}, 0, HatDirection::up}};  // cross (A)
    cfg.buttons[5] = {{SourceKind::button, {}, 1, HatDirection::up}};  // circle (B)
    cfg.buttons[6] = {{SourceKind::button, {}, 2, HatDirection::up}};  // square (X)
    cfg.buttons[7] = {{SourceKind::button, {}, 3, HatDirection::up}};  // triangle (Y)
    cfg.buttons[8] = {{SourceKind::button, {}, 8, HatDirection::up}};  // l1 (L3 / Look Back)
    cfg.buttons[9] = {{SourceKind::button, {}, 9, HatDirection::up}};  // r1 (R3 / Nitro)
    cfg.buttons[10] = {{SourceKind::button, {}, 4, HatDirection::up}}; // l2 (LB / paddle down)
    cfg.buttons[11] = {{SourceKind::button, {}, 5, HatDirection::up}}; // r2 (RB / paddle up)
    cfg.buttons[14] = {{SourceKind::button, {}, 6, HatDirection::up}}; // select (Back)
    cfg.buttons[15] = {{SourceKind::button, {}, 7, HatDirection::up}}; // start (Start)
    cfg.ffb.enabled = false;
    return cfg;
  }

  if (preset_name == "logitech_dfgt") {
    cfg.clutch.source = {};
    // Driving Force GT / Pro: paddles are buttons 4/5 and sequential stick is 12/13.
    cfg.buttons[11] = {{SourceKind::button, {}, 4, HatDirection::up},
                       {SourceKind::button, {}, 12, HatDirection::up}}; // r2 (shift up)
    cfg.buttons[10] = {{SourceKind::button, {}, 5, HatDirection::up},
                       {SourceKind::button, {}, 13, HatDirection::up}}; // l2 (shift down)
    return cfg;
  }

  if (preset_name == "thrustmaster") {
    // Thrustmaster T150/T248/T300RS/TX: brake is typically axis 1, throttle axis 2; paddles are buttons 0/1.
    cfg.throttle.source = {SourceKind::axis, {}, 2, HatDirection::up};
    cfg.brake.source = {SourceKind::axis, {}, 1, HatDirection::up};
    cfg.buttons[10] = {{SourceKind::button, {}, 0, HatDirection::up}}; // l2 (left paddle)
    cfg.buttons[11] = {{SourceKind::button, {}, 1, HatDirection::up}}; // r2 (right paddle)
    cfg.buttons[4] = {{SourceKind::button, {}, 5, HatDirection::up}};  // cross
    cfg.buttons[6] = {{SourceKind::button, {}, 3, HatDirection::up}};  // square
    cfg.buttons[5] = {{SourceKind::button, {}, 4, HatDirection::up}};  // circle
    cfg.buttons[7] = {{SourceKind::button, {}, 2, HatDirection::up}};  // triangle
    return cfg;
  }

  if (preset_name == "fanatec_moza") {
    // Direct Drive wheels & load-cell pedals typically report -32768 rest to +32767 full press.
    cfg.throttle.mode = PedalAxisMode::normal;
    cfg.brake.mode = PedalAxisMode::normal;
    cfg.clutch.mode = PedalAxisMode::normal;
    cfg.ffb.centering_spring = 0.15;
    cfg.ffb.damper = 0.10;
    return cfg;
  }

  if (preset_name == "combined_pedals") {
    // Single-axis combined pedals (270 deg budget wheels / combined HID mode):
    // Axis 1 negative half = throttle, positive half = brake.
    cfg.steering.rotation_degrees = 270;
    cfg.steering.effective_degrees = 270;
    cfg.throttle.source = {SourceKind::axis, {}, 1, HatDirection::up};
    cfg.throttle.mode = PedalAxisMode::combined_negative;
    cfg.brake.source = {SourceKind::axis, {}, 1, HatDirection::up};
    cfg.brake.mode = PedalAxisMode::combined_positive;
    cfg.clutch.source = {};
    return cfg;
  }

  cfg.preset = "custom";
  return cfg;
}

const WheelConfig &default_wheel_config() {
  static const WheelConfig kDefault = [] {
    WheelConfig config = preset_wheel_config("generic");
    config.enabled = false;
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
    const std::string_view raw_line =
        nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
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
            const std::string_view preset = unquote(value, source_name, line_no);
            config.preset = std::string(preset);
          } else if (key == "mode") {
            const std::string_view mode = unquote(value, source_name, line_no);
            if (mode != "pad" && mode != "dfp")
              fail(source_name, line_no, "mode must be \"pad\" or \"dfp\"");
            config.dfp = mode == "dfp";
          } else if (key == "pedal_map_mode") {
            config.pedal_map_mode =
                parse_pedal_map_mode(unquote(value, source_name, line_no), source_name, line_no);
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
          } else if (key == "effective_degrees") {
            const int deg = parse_int_token(value, source_name, line_no, "effective_degrees");
            if (deg < 180 || deg > 1080)
              fail(source_name, line_no, "effective_degrees must be between 180 and 1080");
            config.steering.effective_degrees = deg;
          } else if (key == "deadzone") {
            const double dz = parse_double_token(value, source_name, line_no, "deadzone");
            if (dz < 0.0 || dz > 0.45)
              fail(source_name, line_no, "steering deadzone must be between 0.0 and 0.45");
            config.steering.deadzone = dz;
          } else if (key == "linearity") {
            config.steering.linearity =
                parse_curve_token(unquote(value, source_name, line_no), source_name, line_no);
          } else if (key == "sensitivity") {
            const double sens = parse_double_token(value, source_name, line_no, "sensitivity");
            if (sens < 0.25 || sens > 2.5)
              fail(source_name, line_no, "steering sensitivity must be between 0.25 and 2.5");
            config.steering.sensitivity = sens;
          } else if (key == "invert") {
            config.steering.invert = parse_bool_token(value, source_name, line_no);
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
          } else if (key == "centering_spring") {
            const double s = parse_double_token(value, source_name, line_no, "centering_spring");
            if (s < 0.0 || s > 1.0)
              fail(source_name, line_no, "ffb centering_spring must be between 0.0 and 1.0");
            config.ffb.centering_spring = s;
          } else if (key == "damper") {
            const double d = parse_double_token(value, source_name, line_no, "damper");
            if (d < 0.0 || d > 1.0)
              fail(source_name, line_no, "ffb damper must be between 0.0 and 1.0");
            config.ffb.damper = d;
          } else if (key == "curb_rumble") {
            const double r = parse_double_token(value, source_name, line_no, "curb_rumble");
            if (r < 0.0 || r > 2.0)
              fail(source_name, line_no, "ffb curb_rumble must be between 0.0 and 2.0");
            config.ffb.curb_rumble = r;
          } else if (key == "invert") {
            config.ffb.invert = parse_bool_token(value, source_name, line_no);
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
  out << "# GT4Recomp racing wheel configuration (TASK-018)\n";
  out << "[wheel]\n";
  out << "version = " << config.version << "\n";
  out << "enabled = " << (config.enabled ? "true" : "false") << "\n";
  out << "device_guid = \"" << config.device_guid << "\"\n";
  out << "device_name = \"" << config.device_name << "\"\n";
  out << "preset = \"" << config.preset << "\"\n";
  out << "mode = \"" << (config.dfp ? "dfp" : "pad") << "\"\n";
  out << "pedal_map_mode = \"" << pedal_map_mode_name(config.pedal_map_mode) << "\"\n\n";

  out << "[steering]\n";
  out << "source = \"" << format_source_ref(config.steering.source) << "\"\n";
  out << "rotation_degrees = " << config.steering.rotation_degrees << "\n";
  out << "effective_degrees = " << config.steering.effective_degrees << "\n";
  out << "deadzone = " << config.steering.deadzone << "\n";
  out << "linearity = \"" << curve_name(config.steering.linearity) << "\"\n";
  out << "sensitivity = " << config.steering.sensitivity << "\n";
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
  out << "centering_spring = " << config.ffb.centering_spring << "\n";
  out << "damper = " << config.ffb.damper << "\n";
  out << "curb_rumble = " << config.ffb.curb_rumble << "\n";
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

// The launcher has no wheel page yet and does not pass GT4_WHEEL_FILE, so a wheel.toml beside the launcher's controls file
// (GT4_CONTROLS_FILE, <user>\config) or beside the game executable is used when the variable is unset.
static std::string find_wheel_file() {
  const char *path = std::getenv("GT4_WHEEL_FILE");
  if (path && *path)
    return path;
  std::vector<std::filesystem::path> candidates;
  if (const char *controls = std::getenv("GT4_CONTROLS_FILE"); controls && *controls)
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

WheelConfig load_wheel_from_environment(bool *has_explicit_file) {
  const std::string found = find_wheel_file();
  const char *path = found.c_str();
  if (found.empty()) {
    if (has_explicit_file)
      *has_explicit_file = false;
    WheelConfig disabled = default_wheel_config();
    disabled.enabled = false;
    return disabled;
  }
  if (has_explicit_file)
    *has_explicit_file = true;
  return load_wheel_file(path);
}

std::uint8_t read_steering(const SteeringConfig &config, std::int16_t raw) {
  // Normalize raw signed 16-bit value (-32768..32767) to -1.0..+1.0.
  double signed_norm = raw < 0 ? static_cast<double>(raw) / 32768.0 : static_cast<double>(raw) / 32767.0;
  if (config.invert)
    signed_norm = -signed_norm;

  const double sign = signed_norm < 0.0 ? -1.0 : 1.0;
  double magnitude = std::abs(signed_norm);

  // Scale by physical-to-effective rotation lock ratio (e.g. 540 effective on a 900 deg wheel reaches full lock at 60% travel).
  const int physical_deg = std::clamp(config.rotation_degrees, 180, 1080);
  const int effective_deg = std::clamp(config.effective_degrees, 180, physical_deg);
  const double lock_ratio = static_cast<double>(effective_deg) / static_cast<double>(physical_deg);
  if (lock_ratio > 0.0)
    magnitude /= lock_ratio;

  const double dz = std::clamp(config.deadzone, 0.0, 0.45);
  if (magnitude <= dz)
    return 128;

  double normalized = (magnitude - dz) / (1.0 - dz);
  normalized = std::clamp(normalized, 0.0, 1.0);
  const double curved = apply_curve(config.linearity, normalized);
  const double scaled = std::clamp(curved * std::clamp(config.sensitivity, 0.25, 2.5), 0.0, 1.0);

  if (sign < 0.0) {
    const int offset = static_cast<int>(std::lround(scaled * 128.0));
    return static_cast<std::uint8_t>(std::clamp(128 - offset, 0, 128));
  }
  const int offset = static_cast<int>(std::lround(scaled * 127.0));
  return static_cast<std::uint8_t>(std::clamp(128 + offset, 128, 255));
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
  if (guid.size() < 20)
    return false;
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto parse_u16_le = [&](std::size_t offset) -> int {
    int b0_hi = hex_val(guid[offset]);
    int b0_lo = hex_val(guid[offset + 1]);
    int b1_hi = hex_val(guid[offset + 2]);
    int b1_lo = hex_val(guid[offset + 3]);
    if (b0_lo < 0 || b0_hi < 0 || b1_lo < 0 || b1_hi < 0) return -1;
    const auto byte0 = static_cast<std::uint8_t>((b0_hi << 4) | b0_lo);
    const auto byte1 = static_cast<std::uint8_t>((b1_hi << 4) | b1_lo);
    return static_cast<int>(byte0 | (static_cast<std::uint16_t>(byte1) << 8));
  };
  int parsed_vid = parse_u16_le(8);
  int parsed_pid = parse_u16_le(16);
  if (parsed_vid < 0 || parsed_pid < 0)
    return false;
  vid = static_cast<std::uint16_t>(parsed_vid);
  pid = static_cast<std::uint16_t>(parsed_pid);
  return true;
}

bool is_known_wheel_vendor(std::uint16_t vid) {
  switch (vid) {
  case 0x0F0D: // HORI
  case 0x046D: // Logitech
  case 0x044F: // Thrustmaster
  case 0x0EB7: // Fanatec
  case 0x1209: // Moza
  case 0x3416: // Cammus
  case 0x16D0: // SimuCube
    return true;
  default:
    return false;
  }
}

bool is_candidate_wheel_device(bool sdl_is_wheel_type, bool sdl_is_gamepad,
                               std::string_view device_name, std::string_view guid) {
  (void)sdl_is_gamepad;
  (void)guid; // A USB vendor alone does not establish wheel identity.
  if (sdl_is_wheel_type)
    return true;
  const std::string lower = to_lower_ascii(device_name);
  for (std::string_view auxiliary : {"pedal", "shifter", "handbrake"})
    if (lower.find(auxiliary) != std::string::npos)
      return false;
  constexpr std::array<std::string_view, 16> wheel_names{{
      "wheel", "driving force", "g29", "g920", "g923", "g27", "g25", "t150",
      "t248", "t300", "t500", "t-gt", "apex", "overdrive", "racing base", "wheelbase"}};
  return std::any_of(wheel_names.begin(), wheel_names.end(), [&lower](std::string_view name) {
    return lower.find(name) != std::string::npos;
  });
}

int select_primary_device(const WheelConfig &config, const std::vector<DeviceReading> &devices) {
  if (!config.enabled)
    return -1;
  int selected = -1;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const auto &device = devices[i];
    if (!config.device_guid.empty()) {
      if (to_lower_ascii(device.guid) == to_lower_ascii(config.device_guid)) {
        if (selected >= 0) return -1;
        selected = static_cast<int>(i);
      }
    } else if (is_candidate_wheel_device(device.is_wheel, device.is_gamepad, device.name, device.guid)) {
      return static_cast<int>(i);
    }
  }
  return selected;
}

PadState map_wheel_devices(const WheelConfig &config,
                           const std::vector<DeviceReading> &devices,
                           bool *active) {
  PadState pad{};
  if (active)
    *active = false;
  if (!config.enabled || devices.empty())
    return pad;

  const int primary_idx = select_primary_device(config, devices);
  bool has_explicit_dev_binding = false;
  for (const PedalConfig *p : {&config.throttle, &config.brake, &config.handbrake, &config.clutch}) {
    if (!p->source.device_guid.empty())
      has_explicit_dev_binding = true;
  }
  if (!config.steering.source.device_guid.empty())
    has_explicit_dev_binding = true;
  for (const auto &slot : config.buttons) {
    for (const auto &ref : slot) {
      if (!ref.device_guid.empty())
        has_explicit_dev_binding = true;
    }
  }
  if (primary_idx < 0 && !has_explicit_dev_binding)
    return pad;

  if (active)
    *active = true;

  // 1. Steering -> lstick_x (PS2 axis 2)
  if (!config.steering.source.empty() && config.steering.source.kind == SourceKind::axis) {
    const DeviceReading *steer_dev = resolve_device(config.steering.source, devices, primary_idx);
    if (steer_dev && config.steering.source.index < steer_dev->axes.size()) {
      pad.axes[2] = read_steering(config.steering, steer_dev->axes[config.steering.source.index]);
    }
  }

  // 2. Throttle & Brake pedals
  const std::uint8_t throttle_val = evaluate_pedal_channel(config.throttle, devices, primary_idx);
  const std::uint8_t brake_val = evaluate_pedal_channel(config.brake, devices, primary_idx);

  switch (config.pedal_map_mode) {
  case PedalMapMode::cross_square: {
    if (throttle_val > 0)
      press_pad_button(pad, 14, throttle_val); // Cross
    if (brake_val > 0)
      press_pad_button(pad, 15, brake_val); // Square
    // Also drive rstick_y proportionally so pedals work regardless of GT4 in-game controller type:
    const int net = static_cast<int>(throttle_val) - static_cast<int>(brake_val);
    if (net > 0) {
      const int offset = (net * 128 + 127) / 255;
      pad.axes[1] = static_cast<std::uint8_t>(std::clamp(128 - offset, 0, 128));
    } else if (net < 0) {
      const int offset = ((-net) * 127 + 127) / 255;
      pad.axes[1] = static_cast<std::uint8_t>(std::clamp(128 + offset, 128, 255));
    }
    break;
  }
  case PedalMapMode::l2_r2:
    if (throttle_val > 0)
      press_pad_button(pad, 9, throttle_val); // R2
    if (brake_val > 0)
      press_pad_button(pad, 8, brake_val); // L2
    break;
  case PedalMapMode::both:
    if (throttle_val > 0) {
      press_pad_button(pad, 14, throttle_val); // Cross
      press_pad_button(pad, 9, throttle_val);  // R2
    }
    if (brake_val > 0) {
      press_pad_button(pad, 15, brake_val); // Square
      press_pad_button(pad, 8, brake_val);  // L2
    }
    break;
  case PedalMapMode::rstick_y: {
    // Net throttle vs brake on PS2 rstick_y (axis 1: 0 = full up/throttle, 128 = rest, 255 = full down/brake).
    const int net = static_cast<int>(throttle_val) - static_cast<int>(brake_val);
    if (net > 0) {
      const int offset = (net * 128 + 127) / 255;
      pad.axes[1] = static_cast<std::uint8_t>(std::clamp(128 - offset, 0, 128));
    } else if (net < 0) {
      const int offset = ((-net) * 127 + 127) / 255;
      pad.axes[1] = static_cast<std::uint8_t>(std::clamp(128 + offset, 128, 255));
    }
    break;
  }
  }

  // 3. Handbrake pedal/axis -> Circle (bit 13) with analog pressure
  const std::uint8_t handbrake_val = evaluate_pedal_channel(config.handbrake, devices, primary_idx);
  if (handbrake_val > 0)
    press_pad_button(pad, 13, handbrake_val);

  // 4. Digital buttons, paddles, H-shifter, D-pad hats
  for (std::size_t slot_idx = 0; slot_idx < button_slots.size(); ++slot_idx) {
    const std::uint8_t ps2_bit = button_slots[slot_idx].ps2_bit;
    for (const SourceRef &ref : config.buttons[slot_idx]) {
      if (evaluate_digital_source(ref, devices, primary_idx)) {
        press_pad_button(pad, ps2_bit, 255);
      }
    }
  }

  return pad;
}

void merge_wheel_state(PadState &base, const PadState &wheel_state) {
  base.buttons |= wheel_state.buttons;
  for (std::size_t i = 0; i < base.pressures.size(); ++i)
    base.pressures[i] = std::max(base.pressures[i], wheel_state.pressures[i]);
  for (std::size_t axis = 0; axis < base.axes.size(); ++axis) {
    const int base_dist = std::abs(static_cast<int>(base.axes[axis]) - 128);
    const int wheel_dist = std::abs(static_cast<int>(wheel_state.axes[axis]) - 128);
    if (wheel_dist >= base_dist && wheel_dist > 0)
      base.axes[axis] = wheel_state.axes[axis];
  }
}

WheelFfbCommand compute_ffb_command(const FfbConfig &config, std::uint16_t ps2_rumble) {
  WheelFfbCommand cmd{};
  if (!config.enabled)
    return cmd;

  const double gain = std::clamp(config.strength, 0.0, 2.0);
  const double curb_gain = std::clamp(config.curb_rumble, 0.0, 2.0) * gain;
  cmd.gain_percent = static_cast<int>(std::lround(std::clamp(gain * 100.0, 0.0, 100.0)));
  cmd.autocenter_percent =
      static_cast<int>(std::lround(std::clamp(config.centering_spring * gain * 100.0, 0.0, 100.0)));

  const auto base_motors = gt4::pad::rumble_motors(ps2_rumble);
  const int low_scaled =
      static_cast<int>(std::lround(static_cast<double>(base_motors.low_frequency) * curb_gain));
  const int high_scaled =
      static_cast<int>(std::lround(static_cast<double>(base_motors.high_frequency) * curb_gain));

  cmd.low_frequency = static_cast<std::uint16_t>(std::clamp(low_scaled, 0, 65535));
  cmd.high_frequency = static_cast<std::uint16_t>(std::clamp(high_scaled, 0, 65535));

  const double combined =
      (0.7 * static_cast<double>(cmd.low_frequency) + 0.3 * static_cast<double>(cmd.high_frequency)) / 65535.0;
  cmd.normalized_strength = static_cast<float>(std::clamp(combined, 0.0, 1.0));
  return cmd;
}

WheelInput::WheelInput(WheelConfig config, bool explicit_config, HapticOutput output)
    : config_(std::move(config)), output_override_(std::move(output)), explicit_config_(explicit_config) {
  if (!config_.enabled)
    return;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
    sdl_initialized_ = true;
  } else {
    std::fprintf(stderr, "[wheel] SDL_InitSubSystem(SDL_INIT_JOYSTICK) failed: %s\n", SDL_GetError());
  }
  if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
    haptic_initialized_ = true;
  }
  if (config_.dfp) {
    // The USB capsule plugs the virtual Driving Force Pro in when the IOP's USB driver first touches the controller.
    gt4::usb::DfpBridge::shared().set_enabled(true);
    // Forces go to the wheel the moment the game sends them (same thread: the IOP runs inside the emulation loop),
    // instead of waiting for the next VBlank: up to 16 ms less delay in the game's force loop.
    gt4::usb::DfpBridge::shared().set_force_listener([this] { apply_dfp_forces(dfp_focused_); });
    std::fprintf(stderr, "[wheel] mode dfp: the wheel drives a virtual Driving Force Pro on the emulated USB bus\n");
  }
}

WheelInput::~WheelInput() {
  if (config_.dfp)
    gt4::usb::DfpBridge::shared().set_force_listener({});
  stop();
  close_haptic();
  close_all_devices();
  if (haptic_initialized_)
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
  if (sdl_initialized_)
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

void WheelInput::close_haptic() {
  stop();
  haptic_open_attempted_ = false;
  if (primary_haptic_) {
    if (rumble_initialized_) {
      SDL_StopHapticRumble(primary_haptic_);
      rumble_initialized_ = false;
    }
    dfp_running_ = false;
    dfp_revision_ = ~0ull;
    for (int *id : {&spring_effect_, &damper_effect_, &dfp_constant_, &dfp_spring_, &dfp_damper_, &dfp_friction_}) {
      if (*id >= 0)
        SDL_DestroyHapticEffect(primary_haptic_, *id);
      *id = -1;
    }
    haptic_features_ = 0;
    SDL_CloseHaptic(primary_haptic_);
    primary_haptic_ = nullptr;
  }
}

void WheelInput::open_haptic_for_primary(SDL_Joystick *joystick) {
  close_haptic();
  haptic_open_attempted_ = true;
  if (!haptic_initialized_ || !joystick || !config_.ffb.enabled)
    return;
  primary_haptic_ = SDL_OpenHapticFromJoystick(joystick);
  if (!primary_haptic_)
    return;
  haptic_features_ = SDL_GetHapticFeatures(primary_haptic_);
  // Opening a handle never enables force; focused rumble() resumes the configured outputs.
  if (haptic_features_ & SDL_HAPTIC_GAIN)
    SDL_SetHapticGain(primary_haptic_, 0);
  if (haptic_features_ & SDL_HAPTIC_AUTOCENTER)
    SDL_SetHapticAutocenter(primary_haptic_, 0);
  if (config_.dfp) {
    open_dfp_effects();
    return;
  }
  if (SDL_InitHapticRumble(primary_haptic_))
    rumble_initialized_ = true;
  // Bases without device autocentre (the MOZA R5 reports none) get the centring and damping as
  // DirectInput condition effects on the steering axis instead. Created stopped; resume_condition_effects() runs them.
  const auto make_condition = [&](Uint16 type, double coefficient) -> int {
    if (!(haptic_features_ & type) || coefficient <= 0.0)
      return -1;
    SDL_HapticEffect effect{};
    effect.type = type;
    effect.condition.type = type;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    const auto level = static_cast<Sint16>(std::clamp(coefficient, 0.0, 1.0) * 0x7FFF);
    effect.condition.right_sat[0] = effect.condition.left_sat[0] = 0xFFFF;
    effect.condition.right_coeff[0] = effect.condition.left_coeff[0] = level;
    const int id = SDL_CreateHapticEffect(primary_haptic_, &effect);
    if (id < 0)
      std::fprintf(stderr, "[wheel] haptic effect 0x%04x not created: %s\n", type, SDL_GetError());
    return id;
  };
  spring_effect_ = (haptic_features_ & SDL_HAPTIC_AUTOCENTER) ? -1 : make_condition(SDL_HAPTIC_SPRING, config_.ffb.centering_spring);
  damper_effect_ = make_condition(SDL_HAPTIC_DAMPER, config_.ffb.damper);
  std::fprintf(stderr, "[wheel] force feedback: gain=%d autocenter=%d spring=%s damper=%s rumble=%s\n",
               (haptic_features_ & SDL_HAPTIC_GAIN) ? 1 : 0, (haptic_features_ & SDL_HAPTIC_AUTOCENTER) ? 1 : 0,
               spring_effect_ >= 0 ? "on" : "off", damper_effect_ >= 0 ? "on" : "off",
               rumble_initialized_ ? "on" : "off");
}

void WheelInput::resume_condition_effects() {
  if (!primary_haptic_ || output_override_.stop_all)
    return;
  for (const int id : {spring_effect_, damper_effect_})
    if (id >= 0 && !SDL_RunHapticEffect(primary_haptic_, id, SDL_HAPTIC_INFINITY))
      std::fprintf(stderr, "[wheel] haptic effect %d not started: %s\n", id, SDL_GetError());
}

void WheelInput::close_all_devices() {
  for (auto &dev : devices_) {
    if (dev.joystick) {
      SDL_CloseJoystick(dev.joystick);
      dev.joystick = nullptr;
    }
  }
  devices_.clear();
  primary_joystick_ = nullptr;
}

void WheelInput::scan_devices() {
  if (!sdl_initialized_)
    return;

  // Remove any disconnected joysticks first.
  for (std::size_t i = 0; i < devices_.size();) {
    if (!devices_[i].joystick || !SDL_JoystickConnected(devices_[i].joystick)) {
      if (devices_[i].joystick == primary_joystick_) {
        close_haptic();
        primary_joystick_ = nullptr;
      }
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
    const bool already_open = std::any_of(devices_.begin(), devices_.end(),
                                          [id](const OpenDevice &d) { return d.instance_id == id; });
    if (already_open)
      continue;

    // Identify candidates without opening unrelated hardware handles.
    char candidate_guid[64]{};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), candidate_guid, sizeof(candidate_guid));
    const std::string candidate = to_lower_ascii(candidate_guid);
    const char *candidate_name = SDL_GetJoystickNameForID(id);
    bool referenced = (!config_.device_guid.empty() && to_lower_ascii(config_.device_guid) == candidate);
    const auto source_matches = [&candidate](const SourceRef &source) {
      return !source.device_guid.empty() && to_lower_ascii(source.device_guid) == candidate;
    };
    referenced = referenced || source_matches(config_.steering.source) || source_matches(config_.throttle.source) ||
                 source_matches(config_.brake.source) || source_matches(config_.handbrake.source);
    for (const auto &slot : config_.buttons)
      for (const auto &source : slot) referenced = referenced || source_matches(source);
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
    const std::string name = raw_name ? raw_name : "Unknown USB Device";
    const bool is_wheel_type = SDL_GetJoystickType(joy) == SDL_JOYSTICK_TYPE_WHEEL;
    const bool is_gamepad = SDL_IsGamepad(id);
    const bool is_wheel = is_candidate_wheel_device(is_wheel_type, is_gamepad, name, guid_buf);

    // If no explicit device_guid and not a wheel candidate, keep open only if multi-device GUID matches.
    OpenDevice entry;
    entry.instance_id = id;
    entry.joystick = joy;
    entry.guid = to_lower_ascii(guid_buf);
    entry.name = name;
    entry.is_wheel = is_wheel;
    entry.is_gamepad = is_gamepad;
    devices_.push_back(std::move(entry));
  }
  SDL_free(ids);
}

std::uint32_t WheelInput::primary_instance_id() const {
  return primary_joystick_ ? SDL_GetJoystickID(primary_joystick_) : 0;
}

void WheelInput::poll_and_merge(bool focused, PadState &pad) {
  prepare(focused);
  merge_prepared(pad);
}

void WheelInput::merge_prepared(PadState &pad) const {
  if (prepared_active_)
    merge_wheel_state(pad, prepared_pad_);
}

void WheelInput::prepare(bool focused) {
  prepared_pad_ = {};
  prepared_active_ = false;
  owned_device_ids_.clear();
  if (!config_.enabled || !sdl_initialized_) {
    stop();
    return;
  }
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
    if (device.instance_id == selected_id) resolved = device.joystick;
  if (resolved != primary_joystick_) {
    // Stop the old handle before replacing it; never send old effects to the new device.
    close_haptic();
    primary_joystick_ = resolved;
    if (focused && primary_joystick_) open_haptic_for_primary(primary_joystick_);
  } else if (focused && primary_joystick_ && !haptic_open_attempted_) {
    open_haptic_for_primary(primary_joystick_);
  }
  const auto references = [this](const DeviceReading &device) {
    const auto match = [&device](const SourceRef &source) {
      return !source.device_guid.empty() && to_lower_ascii(source.device_guid) == to_lower_ascii(device.guid);
    };
    if (match(config_.steering.source) || match(config_.throttle.source) || match(config_.brake.source) ||
        match(config_.handbrake.source)) return true;
    for (const auto &slot : config_.buttons)
      for (const auto &source : slot)
        if (match(source)) return true;
    return false;
  };
  for (const auto &device : readings)
    if (device.instance_id == selected_id || (references(device) &&
        std::count_if(readings.begin(), readings.end(), [&device](const DeviceReading &candidate) {
          return candidate.guid == device.guid;
        }) == 1)) owned_device_ids_.push_back(device.instance_id);
  if (config_.dfp) {
    prepare_dfp(focused, readings, selected);
    if (!focused)
      stop();
    return;
  }
  if (!focused) {
    stop();
    return;
  }
  prepared_pad_ = map_wheel_devices(config_, readings, &prepared_active_);
}

// ------------------------------------------------------------------------------------------------- DFP mode (TASK-018)

void WheelInput::prepare_dfp(bool focused, const std::vector<DeviceReading> &readings, int primary) {
  dfp_focused_ = focused;
  gt4::usb::DfpBridge &bridge = gt4::usb::DfpBridge::shared();
  gt4::usb::DfpInput input;  // neutral: centred, nothing pressed
  if (focused && (primary >= 0 || !readings.empty())) {
    // Steering: the wheel's physical rotation maps 1:1 onto the range the game set (200 or 900 degrees).
    if (config_.steering.source.kind == SourceKind::axis) {
      const DeviceReading *device = resolve_device(config_.steering.source, readings, primary);
      if (device && config_.steering.source.index < device->axes.size()) {
        const std::int16_t raw = device->axes[config_.steering.source.index];
        double position = raw < 0 ? raw / 32768.0 : raw / 32767.0;
        if (config_.steering.invert)
          position = -position;
        const double degrees = position * std::clamp(config_.steering.rotation_degrees, 180, 1080) / 2.0;
        const double half_range = std::max(bridge.forces().range_degrees, 40) / 2.0;
        const double scaled = std::clamp(degrees / half_range, -1.0, 1.0);
        input.steering = static_cast<std::uint16_t>(std::lround(8191.5 + scaled * 8191.5));
      }
    }
    input.throttle = evaluate_pedal_channel(config_.throttle, readings, primary);
    input.brake = evaluate_pedal_channel(config_.brake, readings, primary);
    // Buttons: the configured DualShock 2 slots, moved to the DFP's bits. The shift slots (L2/R2 here) become the DFP's
    // gear paddles (L1/R1 on the DFP), and the D-pad slots become its hat.
    const auto pressed = [&](const char *slot_name) {
      for (std::size_t slot = 0; slot < button_slots.size(); ++slot)
        if (std::string_view(button_slots[slot].name) == slot_name)
          for (const SourceRef &source : config_.buttons[slot])
            if (evaluate_digital_source(source, readings, primary))
              return true;
      return false;
    };
    static constexpr std::pair<const char *, unsigned> kButtonBits[] = {
        {"cross", 0}, {"square", 1}, {"circle", 2}, {"triangle", 3}, {"r2", 4}, {"l2", 5}, {"r1", 6}, {"l1", 7},
        {"select", 8}, {"start", 9}, {"r3", 10}, {"l3", 11}};
    for (const auto &[name, bit] : kButtonBits)
      if (pressed(name))
        input.buttons |= static_cast<std::uint16_t>(1u << bit);
    if (evaluate_pedal_channel(config_.handbrake, readings, primary) > 128)
      input.buttons |= 1u << 2;  // the handbrake pedal presses Circle, as on the pad path
    const bool up = pressed("dpad_up"), right = pressed("dpad_right"), down = pressed("dpad_down"),
               left = pressed("dpad_left");
    if (up && right) input.hat = 1;
    else if (right && down) input.hat = 3;
    else if (down && left) input.hat = 5;
    else if (left && up) input.hat = 7;
    else if (up) input.hat = 0;
    else if (right) input.hat = 2;
    else if (down) input.hat = 4;
    else if (left) input.hat = 6;
  }
  bridge.set_input(input);
}

void WheelInput::open_dfp_effects() {
  // Created stopped with zero strength; apply_dfp_forces() runs them once and afterwards only updates them.
  const auto create = [&](Uint16 type) -> int {
    if (!(haptic_features_ & type))
      return -1;
    SDL_HapticEffect effect{};
    effect.type = type;
    if (type == SDL_HAPTIC_CONSTANT) {
      effect.constant.type = type;
      effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
      effect.constant.direction.dir[0] = 1;
      effect.constant.length = SDL_HAPTIC_INFINITY;
    } else {
      effect.condition.type = type;
      effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
      effect.condition.direction.dir[0] = 1;
      effect.condition.length = SDL_HAPTIC_INFINITY;
    }
    const int id = SDL_CreateHapticEffect(primary_haptic_, &effect);
    if (id < 0)
      std::fprintf(stderr, "[wheel] DFP effect 0x%04x not created: %s\n", type, SDL_GetError());
    return id;
  };
  dfp_constant_ = create(SDL_HAPTIC_CONSTANT);
  dfp_spring_ = create(SDL_HAPTIC_SPRING);
  dfp_damper_ = create(SDL_HAPTIC_DAMPER);
  dfp_friction_ = create(SDL_HAPTIC_FRICTION);
  dfp_running_ = false;
  dfp_revision_ = ~0ull;
  std::fprintf(stderr, "[wheel] DFP force feedback on the wheel: constant=%s spring=%s damper=%s friction=%s\n",
               dfp_constant_ >= 0 ? "yes" : "no", dfp_spring_ >= 0 ? "yes" : "no", dfp_damper_ >= 0 ? "yes" : "no",
               dfp_friction_ >= 0 ? "yes" : "no");
}

void WheelInput::apply_dfp_forces(bool focused) {
  if (!focused || !config_.ffb.enabled || !primary_haptic_) {
    stop();
    return;
  }
  const gt4::usb::DfpForces forces = gt4::usb::DfpBridge::shared().forces();
  if (dfp_running_ && forces.revision == dfp_revision_)
    return;
  // A summary of what the game asks for, at most every two seconds.
  const std::uint64_t now_ms = SDL_GetTicks();
  if (now_ms >= dfp_next_log_ms_) {
    dfp_next_log_ms_ = now_ms + 2000;
    std::fprintf(stderr,
                 "[wheel] DFP forces rev %llu: constant %+.2f spring %d (%.2f/%.2f sat %.2f) damper %d (%.2f) friction %d "
                 "autocentre %d (%.2f) range %d\n",
                 static_cast<unsigned long long>(forces.revision), forces.constant, forces.spring ? 1 : 0,
                 forces.spring_left, forces.spring_right, forces.spring_saturation, forces.damper ? 1 : 0,
                 forces.damper_left, forces.friction ? 1 : 0, forces.autocenter ? 1 : 0, forces.autocenter_strength,
                 forces.range_degrees);
  }
  const double gain = std::clamp(config_.ffb.strength, 0.0, 2.0);
  // The damping below is the wheel's own stabiliser, so it does not follow the force strength.
  double active_gain = gain;
  const auto level = [&](double value) {
    return static_cast<Sint16>(std::lround(std::clamp(value * active_gain, -1.0, 1.0) * 32767.0));
  };
  const auto saturation = [](double value) { return static_cast<Uint16>(std::lround(std::clamp(value, 0.0, 1.0) * 65535.0)); };
  const auto update_condition = [&](int id, Uint16 type, double left, double right, double center, double deadband,
                                    double sat) {
    if (id < 0)
      return;
    SDL_HapticEffect effect{};
    effect.type = type;
    effect.condition.type = type;
    effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.condition.direction.dir[0] = 1;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.left_coeff[0] = level(left);
    effect.condition.right_coeff[0] = level(right);
    effect.condition.left_sat[0] = effect.condition.right_sat[0] = saturation(sat);
    effect.condition.center[0] = static_cast<Sint16>(std::lround(std::clamp(center, -1.0, 1.0) * 32767.0));
    effect.condition.deadband[0] = saturation(deadband / 2.0);
    if (!SDL_UpdateHapticEffect(primary_haptic_, id, &effect) && dfp_errors_++ < 20)
      std::fprintf(stderr, "[wheel] DFP update of effect %d (0x%04x) failed: %s\n", id, type, SDL_GetError());
  };
  if (dfp_constant_ >= 0) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    effect.constant.direction.dir[0] = 1;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = level(config_.ffb.invert ? -forces.constant : forces.constant);
    if (!SDL_UpdateHapticEffect(primary_haptic_, dfp_constant_, &effect) && dfp_errors_++ < 20)
      std::fprintf(stderr, "[wheel] DFP update of the constant force failed: %s\n", SDL_GetError());
  }
  // The game's spring; without one, the DFP's own default centring spring while it is on (the MOZA has no device
  // autocentre to stand in for it).
  if (forces.spring)
    update_condition(dfp_spring_, SDL_HAPTIC_SPRING, forces.spring_left, forces.spring_right, forces.spring_center,
                     forces.spring_deadband, forces.spring_saturation);
  else if (forces.autocenter)
    update_condition(dfp_spring_, SDL_HAPTIC_SPRING, forces.autocenter_strength, forces.autocenter_strength, 0, 0, 1);
  else
    update_condition(dfp_spring_, SDL_HAPTIC_SPRING, 0, 0, 0, 0, 0);
  // Damping: the game's damper, at least the configured floor ([ffb] damper). The wheel's firmware applies it with no
  // game latency; a geared DFP had some of this from its own friction, a direct-drive base has almost none.
  const double damping_floor =
      gain > 0.0 ? config_.ffb.damper : 0.0;
  active_gain = 1.0;
  update_condition(dfp_damper_, SDL_HAPTIC_DAMPER, std::max(forces.damper ? forces.damper_left : 0.0, damping_floor),
                   std::max(forces.damper ? forces.damper_right : 0.0, damping_floor), 0, 0,
                   forces.damper ? std::max(forces.damper_saturation, damping_floor > 0 ? 1.0 : 0.0) : 1.0);
  active_gain = gain;
  update_condition(dfp_friction_, SDL_HAPTIC_FRICTION, forces.friction ? forces.friction_coefficient : 0,
                   forces.friction ? forces.friction_coefficient : 0, 0, 0, forces.friction_saturation);
  if (!dfp_running_) {
    if (haptic_features_ & SDL_HAPTIC_GAIN)
      SDL_SetHapticGain(primary_haptic_, 100);
    for (const int id : {dfp_constant_, dfp_spring_, dfp_damper_, dfp_friction_})
      if (id >= 0)
        if (!SDL_RunHapticEffect(primary_haptic_, id, SDL_HAPTIC_INFINITY))
          std::fprintf(stderr, "[wheel] DFP effect %d not started: %s\n", id, SDL_GetError());
    if (dfp_starts_++ < 10)
      std::fprintf(stderr, "[wheel] DFP effects running (start %llu)\n", static_cast<unsigned long long>(dfp_starts_));
    dfp_running_ = true;
  }
  dfp_revision_ = forces.revision;
}

HapticOutput WheelInput::haptic_output() {
  if (output_override_.gain || output_override_.autocenter || output_override_.stop_all ||
      output_override_.stop_rumble || output_override_.rumble || output_override_.joystick_rumble)
    return output_override_;
  HapticOutput output;
  // Every force goes through this one SDL haptic handle. The 1.2.x bridge also called SDL_RumbleJoystick,
  // which on a DirectInput wheel drives a second effect on the same device; with both active the MOZA R5 crashed the
  // game inside pid.dll (access violation, 2026-11-05). Features the device lacks are never called.
  output.gain = [this](int value) {
    if (primary_haptic_ && (haptic_features_ & SDL_HAPTIC_GAIN)) SDL_SetHapticGain(primary_haptic_, value);
  };
  output.autocenter = [this](int value) {
    if (primary_haptic_ && (haptic_features_ & SDL_HAPTIC_AUTOCENTER)) SDL_SetHapticAutocenter(primary_haptic_, value);
  };
  output.stop_all = [this] { if (primary_haptic_) SDL_StopHapticEffects(primary_haptic_); };
  output.stop_rumble = [this] { if (primary_haptic_ && rumble_initialized_) SDL_StopHapticRumble(primary_haptic_); };
  output.rumble = [this](float value) { if (primary_haptic_ && rumble_initialized_) SDL_PlayHapticRumble(primary_haptic_, value, 500); };
  return output;
}

void stop_haptic_output(const HapticOutput &output) {
  if (output.autocenter) output.autocenter(0);
  if (output.stop_all) output.stop_all();
  if (output.stop_rumble) output.stop_rumble();
  if (output.gain) output.gain(0);
  if (output.joystick_rumble) output.joystick_rumble(0, 0);
}

void WheelInput::rumble(bool focused, std::uint16_t ps2_rumble) {
  if (config_.enabled && config_.dfp) {  // the game's own force feedback replaces the pad rumble
    apply_dfp_forces(focused);
    return;
  }
  if (!focused || !config_.enabled || !config_.ffb.enabled || !primary_joystick_) {
    stop();
    return;
  }
  const auto output = haptic_output();
  const std::uint64_t now_ms = SDL_GetTicks();
  const WheelFfbCommand command = compute_ffb_command(config_.ffb, ps2_rumble);
  if (haptic_suspended_) {
    if (output.gain) output.gain(command.gain_percent);
    if (output.autocenter) output.autocenter(command.autocenter_percent);
    resume_condition_effects();
    haptic_suspended_ = false;
  }
  if (ps2_rumble == last_rumble_raw_ && (ps2_rumble == 0 || now_ms - last_rumble_ms_ < 250)) return;
  last_rumble_raw_ = ps2_rumble;
  last_rumble_ms_ = now_ms;
  if (command.normalized_strength <= 0.0f) {
    if (output.stop_rumble) output.stop_rumble();
  } else if (output.rumble) output.rumble(command.normalized_strength);
  // Only an injected output (the fixtures) has a joystick rumble sink; production uses the haptic rumble above.
  if (output.joystick_rumble) output.joystick_rumble(command.low_frequency, command.high_frequency);
}

void WheelInput::stop() {
  // Autocenter is independent of rumble: this must run even when the last game rumble word was zero.
  stop_haptic_output(haptic_output());
  haptic_suspended_ = true;
  dfp_running_ = false;  // stop_all stopped the DFP effects too; they run again on the next focused update
  last_rumble_raw_ = 0;
  last_rumble_ms_ = 0;
}

} // namespace gt4::wheel
