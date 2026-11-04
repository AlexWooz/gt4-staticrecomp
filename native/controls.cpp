#include "controls.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gt4::controls {
namespace {

// XInput stick and trigger geometry. Deadzones are the XInput constants.
constexpr int trigger_full = 255;
constexpr int trigger_default_deadzone = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
constexpr int stick_positive_full = 32767;
constexpr int stick_negative_full = 32768;
// TASK-147: 10% of the travel on both sticks. They were the XInput constants 7849 and 8689 (24% and 26.5%), which stacked
// on the game's own stick dead zone (game_stick_byte below): nothing up to about 46% of the travel, then 1.9 times too steep.
constexpr int left_stick_deadzone = 3277;
constexpr int right_stick_deadzone = 3277;

// TASK-147: GT4 turns the pressure byte p of the button assigned to a function into min(100 * min(2p, 256) / d, 256),
// with d = 35 for the throttle and 65 for the brake (v2.00 0x0043BEE8 and 0x0043BF48, Spec II 0x00319194 and 0x003191F0,
// read in the translated code; Confirmed): 100% throttle from p = 45 and 100% brake from p = 84, made for the DualShock 2's
// pressure buttons. A trigger that drives the whole byte reached 100% throttle at about 27% of its travel and 100% brake at
// about 40% (owner and players: "full at a third of the trigger"). A trigger on R2, the throttle in the usual trigger setup,
// now spans 0..45 over its travel and one on L2, the brake, 0..84: 100% at the end of the travel, with the game's own steps
// (46 for the throttle, 85 for the brake, as many as a DualShock 2 gives). The option `range` changes it per binding.
constexpr std::uint8_t r2_pressure_full = 45;
constexpr std::uint8_t l2_pressure_full = 84;

// TASK-147: GT4's remapper turns a stick byte b into out = (128b + 45) / 90 below 90, 128 from 90 to 165, and
// 128 + (127(b - 165) + 45) / 90 above 165 (integer division; the knot table, 0, 90, 165, 255 to 0, 128, 128, 255, is at
// v2.00 0x0064C4D8 and Spec II 0x00681088, byte for byte the same; Confirmed): a dead zone of 29% of each half, and steering is
// -32 (out - 128). A reading `steps` (1..127 to the right, 1..128 to the left) away from rest becomes the byte past that
// band in the same proportion, 165 + round(90 steps / 127) or 90 - round(90 steps / 128), so the game sees this binding's
// own linear reading with no second dead zone: from the first step, to the nearest of the game's steps (it has 90 bytes
// for 127 steps beyond its band, so out - 128 is within one step of `steps`). The right stick as throttle or brake (curve
// 37..127 of its one-sided magnitude) starts from the same bytes, 89 and 166.
std::uint8_t game_stick_byte(int steps, bool negative) {
  if (steps <= 0)
    return 128;
  if (!negative)
    return static_cast<std::uint8_t>(165 + (180 * steps + 127) / 254);
  return static_cast<std::uint8_t>(90 - (180 * steps + 128) / 256);
}

std::uint8_t default_pressure_full(std::string_view input_name) {
  if (input_name == "r2")
    return r2_pressure_full;
  if (input_name == "l2")
    return l2_pressure_full;
  return static_cast<std::uint8_t>(trigger_full);
}

// Virtual-key codes (Win32 VK_*), listed here so this file needs no Windows headers.
constexpr std::uint8_t vk_back = 0x08, vk_tab = 0x09, vk_return = 0x0D, vk_shift = 0x10,
                       vk_control = 0x11, vk_menu = 0x12, vk_escape = 0x1B, vk_space = 0x20,
                       vk_prior = 0x21, vk_next = 0x22, vk_end = 0x23, vk_home = 0x24,
                       vk_left = 0x25, vk_up = 0x26, vk_right = 0x27, vk_down = 0x28,
                       vk_insert = 0x2D, vk_delete = 0x2E, vk_numpad0 = 0x60,
                       vk_f1 = 0x70, vk_f7 = 0x76, vk_f8 = 0x77, vk_f9 = 0x78, vk_f11 = 0x7A;

std::string trim(std::string_view text) {
  std::size_t first = 0;
  std::size_t last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
    ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    --last;
  return std::string(text.substr(first, last - first));
}

std::string lower(std::string_view text) {
  std::string result(text);
  for (char &character : result)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

[[noreturn]] void fail(const std::string &source, std::size_t line, const std::string &message) {
  throw ConfigError(source + ":" + std::to_string(line) + ": " + message);
}

std::size_t input_index(std::string_view name) {
  for (std::size_t index = 0; index < input_count; ++index)
    if (name == inputs[index].name)
      return index;
  return input_count;
}

// Default deadzone of a source, as a fraction of its full travel.
double default_deadzone(SourceKind kind, std::uint8_t source_index) {
  if (kind == SourceKind::xinput_trigger)
    return static_cast<double>(trigger_default_deadzone) / trigger_full;
  // Slots 0 and 1 are the right stick (RX, RY); 2 and 3 the left stick (LX, LY).
  const int raw = source_index < 2 ? right_stick_deadzone : left_stick_deadzone;
  return static_cast<double>(raw) / stick_positive_full;
}

// Deadzone as an integer in raw units, rounded to the nearest unit.
int deadzone_raw(const Binding &binding, int full) {
  return static_cast<int>(std::lround(binding.deadzone * full));
}

double shape(double value, Curve curve) {
  switch (curve) {
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
  case Curve::linear:
  default:
    return value;
  }
}

// The default shape (linear, scale 1) uses integer arithmetic, which reproduces the
// slice 157 mapping bit for bit. Other shapes use doubles.
bool is_default_shape(const Binding &binding) {
  return binding.scale == 1.0 && binding.curve == Curve::linear;
}

// ---- source table ---------------------------------------------------------------

struct XInputSource {
  const char *name;
  SourceKind kind;
  std::uint16_t button_mask;
  std::uint8_t source_index;
};

constexpr XInputSource xinput_sources[] = {
    {"a", SourceKind::xinput_button, xinput::a, 0},
    {"b", SourceKind::xinput_button, xinput::b, 0},
    {"x", SourceKind::xinput_button, xinput::x, 0},
    {"y", SourceKind::xinput_button, xinput::y, 0},
    {"lb", SourceKind::xinput_button, xinput::left_shoulder, 0},
    {"rb", SourceKind::xinput_button, xinput::right_shoulder, 0},
    {"back", SourceKind::xinput_button, xinput::back, 0},
    {"start", SourceKind::xinput_button, xinput::start, 0},
    {"lthumb", SourceKind::xinput_button, xinput::left_thumb, 0},
    {"rthumb", SourceKind::xinput_button, xinput::right_thumb, 0},
    {"dpadup", SourceKind::xinput_button, xinput::dpad_up, 0},
    {"dpaddown", SourceKind::xinput_button, xinput::dpad_down, 0},
    {"dpadleft", SourceKind::xinput_button, xinput::dpad_left, 0},
    {"dpadright", SourceKind::xinput_button, xinput::dpad_right, 0},
    {"lt", SourceKind::xinput_trigger, 0, 0},
    {"rt", SourceKind::xinput_trigger, 0, 1},
    {"rx", SourceKind::xinput_axis, 0, 0},
    {"ry", SourceKind::xinput_axis, 0, 1},
    {"lx", SourceKind::xinput_axis, 0, 2},
    {"ly", SourceKind::xinput_axis, 0, 3},
};

struct KeySource {
  const char *name;
  std::uint8_t virtual_key;
};

// Letters and digits are added in code; this table holds the named keys.
constexpr KeySource named_keys[] = {
    {"enter", vk_return},  {"backspace", vk_back},  {"tab", vk_tab},
    {"space", vk_space},   {"shift", vk_shift},     {"ctrl", vk_control},
    {"control", vk_control}, {"alt", vk_menu},      {"esc", vk_escape},
    {"up", vk_up},         {"down", vk_down},       {"left", vk_left},
    {"right", vk_right},   {"insert", vk_insert},   {"delete", vk_delete},
    {"home", vk_home},     {"end", vk_end},         {"pageup", vk_prior},
    {"pagedown", vk_next},
};

// Resolves a key name (case-insensitive) to a VK code. Returns false when unknown.
bool resolve_key(const std::string &name, std::uint8_t &virtual_key) {
  if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
    virtual_key = static_cast<std::uint8_t>('A' + (name[0] - 'a'));
    return true;
  }
  if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
    virtual_key = static_cast<std::uint8_t>('0' + (name[0] - '0'));
    return true;
  }
  if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
    const int number = std::atoi(name.c_str() + 1);
    if (number >= 1 && number <= 12) {
      virtual_key = static_cast<std::uint8_t>(vk_f1 + number - 1);
      return true;
    }
  }
  if (name.size() == 4 && name.compare(0, 3, "num") == 0 && name[3] >= '0' && name[3] <= '9') {
    virtual_key = static_cast<std::uint8_t>(vk_numpad0 + (name[3] - '0'));
    return true;
  }
  for (const auto &key : named_keys)
    if (name == key.name) {
      virtual_key = key.virtual_key;
      return true;
    }
  return false;
}

// Keys the host reads for its own shortcuts (window.cpp): F7/F8/F9 for the probe,
// F11 for fullscreen. Alt+Enter is handled by the host and is allowed as a binding.
bool is_reserved_key(std::uint8_t virtual_key) {
  return virtual_key == vk_f7 || virtual_key == vk_f8 || virtual_key == vk_f9 ||
         virtual_key == vk_f11;
}

double parse_number(const std::string &text, const std::string &source, std::size_t line,
                    const std::string &option) {
  if (text.empty())
    fail(source, line, "option " + option + " needs a number");
  errno = 0;
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(value) || errno == ERANGE)
    fail(source, line, "option " + option + " is not a number: " + text);
  return value;
}

// Parses "source | option=value option=value" for one binding of `input`.
Binding parse_source(std::string_view text, const InputInfo &input, const std::string &source,
                     std::size_t line) {
  Binding binding;
  const std::size_t bar = text.find('|');
  const std::string_view source_text = bar == std::string_view::npos ? text : text.substr(0, bar);
  const std::string options_text =
      bar == std::string_view::npos ? std::string() : trim(text.substr(bar + 1));
  if (bar != std::string_view::npos && options_text.find('|') != std::string::npos)
    fail(source, line, "only one '|' is allowed in a binding");

  const std::string trimmed = trim(source_text);
  const std::size_t colon = trimmed.find(':');
  if (colon == std::string::npos)
    fail(source, line, "source must look like xinput:A or key:Z, got '" + trimmed + "'");
  const std::string kind = lower(trim(std::string_view(trimmed).substr(0, colon)));
  const std::string name = lower(trim(std::string_view(trimmed).substr(colon + 1)));

  if (kind == "xinput") {
    const XInputSource *match = nullptr;
    for (const auto &candidate : xinput_sources)
      if (name == candidate.name)
        match = &candidate;
    if (!match)
      fail(source, line, "unknown XInput source '" + name + "'");
    binding.kind = match->kind;
    binding.button_mask = match->button_mask;
    binding.source_index = match->source_index;
  } else if (kind == "key") {
    std::uint8_t virtual_key = 0;
    if (!resolve_key(name, virtual_key))
      fail(source, line, "unknown key '" + trim(std::string_view(trimmed).substr(colon + 1)) + "'");
    if (is_reserved_key(virtual_key))
      fail(source, line, "key '" + name + "' is reserved for a host shortcut");
    binding.kind = SourceKind::keyboard;
    binding.virtual_key = virtual_key;
  } else {
    fail(source, line, "unknown source kind '" + kind + "' (use xinput or key)");
  }

  if (input.axis && binding.kind != SourceKind::xinput_axis)
    fail(source, line, std::string("input ") + input.name +
                           " is a stick axis: bind it to xinput:LX, LY, RX or RY");
  if (!input.axis && binding.kind == SourceKind::xinput_axis)
    fail(source, line, std::string("input ") + input.name +
                           " is a button: a stick cannot drive it");

  if (binding.kind == SourceKind::xinput_button || binding.kind == SourceKind::keyboard) {
    if (!options_text.empty())
      fail(source, line, "digital sources take no options");
    return binding;
  }
  binding.deadzone = default_deadzone(binding.kind, binding.source_index);
  if (binding.kind == SourceKind::xinput_trigger)
    binding.pressure_full = default_pressure_full(input.name);

  std::istringstream tokens(options_text);
  std::string token;
  while (tokens >> token) {
    const std::size_t equals = token.find('=');
    if (equals == std::string::npos)
      fail(source, line, "option must look like name=value, got '" + token + "'");
    const std::string option = lower(token.substr(0, equals));
    const std::string value = token.substr(equals + 1);
    if (option == "deadzone") {
      const double fraction = parse_number(value, source, line, option);
      if (fraction < 0.0 || fraction >= 1.0)
        fail(source, line, "deadzone must be at least 0 and below 1");
      binding.deadzone = fraction;
    } else if (option == "scale") {
      const double scale = parse_number(value, source, line, option);
      if (scale < 0.0 || scale > 2.0)
        fail(source, line, "scale must be between 0 and 2");
      binding.scale = scale;
    } else if (option == "curve") {
      const std::string curve = lower(value);
      if (curve == "linear")
        binding.curve = Curve::linear;
      else if (curve == "quadratic")
        binding.curve = Curve::quadratic;
      else if (curve == "cubic")
        binding.curve = Curve::cubic;
      else if (curve == "s_curve" || curve == "scurve" || curve == "s-curve")
        binding.curve = Curve::s_curve;
      else if (curve == "smooth_power" || curve == "smooth")
        binding.curve = Curve::smooth_power;
      else if (curve == "sublinear" || curve == "root")
        binding.curve = Curve::sublinear;
      else if (curve == "logarithmic" || curve == "log")
        binding.curve = Curve::logarithmic;
      else
        fail(source, line, "curve must be linear, quadratic, cubic, s_curve, smooth_power, sublinear or logarithmic");
    } else if (option == "range") {
      if (binding.kind != SourceKind::xinput_trigger)
        fail(source, line, "range applies to triggers only");
      const double full = parse_number(value, source, line, option);
      if (full < 1.0 || full > 255.0 || full != std::floor(full))
        fail(source, line, "range must be a whole number from 1 to 255");
      binding.pressure_full = static_cast<std::uint8_t>(full);
    } else if (option == "game_deadzone") {
      if (binding.kind != SourceKind::xinput_axis)
        fail(source, line, "game_deadzone applies to stick axes only");
      const std::string mode = lower(value);
      if (mode != "skip" && mode != "keep")
        fail(source, line, "game_deadzone must be skip or keep");
      binding.skip_game_deadzone = mode == "skip";
    } else if (option == "invert") {
      if (binding.kind != SourceKind::xinput_axis)
        fail(source, line, "invert applies to stick axes only");
      const std::string flag = lower(value);
      if (flag != "true" && flag != "false")
        fail(source, line, "invert must be true or false");
      binding.invert = flag == "true";
    } else {
      fail(source, line, "unknown option '" + option + "'");
    }
  }
  return binding;
}

Binding button(std::uint16_t mask) {
  Binding binding;
  binding.kind = SourceKind::xinput_button;
  binding.button_mask = mask;
  return binding;
}

Binding trigger(std::uint8_t index, std::string_view input_name) {
  Binding binding;
  binding.kind = SourceKind::xinput_trigger;
  binding.source_index = index;
  binding.deadzone = default_deadzone(SourceKind::xinput_trigger, index);
  binding.pressure_full = default_pressure_full(input_name);
  return binding;
}

Binding stick(std::uint8_t slot, bool invert) {
  Binding binding;
  binding.kind = SourceKind::xinput_axis;
  binding.source_index = slot;
  binding.deadzone = default_deadzone(SourceKind::xinput_axis, slot);
  binding.invert = invert;
  return binding;
}

Binding key(std::uint8_t virtual_key) {
  Binding binding;
  binding.kind = SourceKind::keyboard;
  binding.virtual_key = virtual_key;
  return binding;
}

// Parses a value: a single quoted string, or a one-line array of quoted strings.
std::vector<std::string> parse_source_list(std::string_view value, const std::string &source,
                                           std::size_t line) {
  std::vector<std::string> sources;
  const std::string text = trim(value);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    const std::string inner = text.substr(1, text.size() - 2);
    if (inner.find('"') != std::string::npos)
      fail(source, line, "quoted strings cannot contain '\"'");
    sources.push_back(inner);
    return sources;
  }
  if (text.size() < 2 || text.front() != '[' || text.back() != ']')
    fail(source, line, "value must be a quoted string or an array of quoted strings");
  std::size_t position = 1;
  const std::size_t end = text.size() - 1;
  while (true) {
    while (position < end && std::isspace(static_cast<unsigned char>(text[position])))
      ++position;
    if (position == end)
      break;
    if (text[position] != '"')
      fail(source, line, "array items must be quoted strings");
    const std::size_t close = text.find('"', position + 1);
    if (close == std::string::npos || close >= end)
      fail(source, line, "unterminated string in array");
    sources.push_back(text.substr(position + 1, close - position - 1));
    position = close + 1;
    while (position < end && std::isspace(static_cast<unsigned char>(text[position])))
      ++position;
    if (position == end)
      break;
    if (text[position] != ',')
      fail(source, line, "expected ',' between array items");
    ++position;
  }
  return sources;
}

// Removes a '#' comment that is not inside a quoted string.
std::string_view strip_comment(std::string_view line) {
  bool quoted = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (line[index] == '"')
      quoted = !quoted;
    else if (line[index] == '#' && !quoted)
      return line.substr(0, index);
  }
  return line;
}

} // namespace

const ControlMap &default_controls() {
  static const ControlMap defaults = [] {
    ControlMap map;
    auto set = [&map](std::string_view name, std::vector<Binding> bindings) {
      map.bindings[input_index(name)] = std::move(bindings);
    };
    // Pad sources use the XInput names, which name the physical positions: SDL3 (TASK-075) reads the same
    // positions on every pad (A = SOUTH, B = EAST, X = WEST, Y = NORTH).
    // Keyboard keys are the TASK-076 layout (docs/CONTROLS.md, keyboard-default): several keys per button.
    set("dpad_up", {button(xinput::dpad_up), key(vk_up), key('E')});
    set("dpad_right", {button(xinput::dpad_right), key('D'), key(vk_right)});
    set("dpad_down", {button(xinput::dpad_down), key(vk_down)});
    set("dpad_left", {button(xinput::dpad_left), key('A'), key(vk_left)});
    set("cross", {button(xinput::a), key('W'), key(vk_return)});
    set("circle", {button(xinput::b), key(vk_space)});
    set("square", {button(xinput::x), key('S')});
    set("triangle", {button(xinput::y), key(vk_back), key(vk_escape)});
    // L1 is look back (rear view); R1 is nitro (boost); L2 and R2 are gear down and gear up.
    set("l1", {button(xinput::left_shoulder), key('Q')});
    set("r1", {button(xinput::right_shoulder), key(vk_shift)});
    // Triggers are pressure-sensitive L2/R2, like PCSX2's default XInput map.
    set("l2", {trigger(0, "l2"), key('F')});
    set("r2", {trigger(1, "r2"), key('R')});
    set("l3", {button(xinput::left_thumb)});
    set("r3", {button(xinput::right_thumb)});
    // Select is the camera (C or Tab).
    set("select", {button(xinput::back), key('C'), key(vk_tab)});
    // Start has no key in the TASK-076 table (Enter is cross); P is a hypothesis pending the owner's choice.
    set("start", {button(xinput::start), key('P')});
    set("rstick_x", {stick(0, false)});
    set("rstick_y", {stick(1, true)});
    set("lstick_x", {stick(2, false)});
    set("lstick_y", {stick(3, true)});
    return map;
  }();
  return defaults;
}

ControlMap parse_controls(std::string_view text, const std::string &source_name) {
  ControlMap map = default_controls();
  bool schema_seen = false;
  bool in_bindings = false;
  std::vector<bool> replaced(input_count, false);
  std::size_t line_number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const std::size_t newline = text.find('\n', position);
    std::string_view raw_line = newline == std::string_view::npos
                                    ? text.substr(position)
                                    : text.substr(position, newline - position);
    position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    ++line_number;
    if (!raw_line.empty() && raw_line.back() == '\r')
      raw_line.remove_suffix(1);
    const std::string line = trim(strip_comment(raw_line));
    if (line.empty())
      continue;
    if (line.front() == '[') {
      if (line != "[bindings]")
        fail(source_name, line_number, "unknown table " + line + "; only [bindings] is defined");
      in_bindings = true;
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos)
      fail(source_name, line_number, "expected name = value");
    const std::string key_name = trim(std::string_view(line).substr(0, equals));
    const std::string value = trim(std::string_view(line).substr(equals + 1));
    if (key_name.empty())
      fail(source_name, line_number, "missing name before '='");
    if (!in_bindings) {
      if (key_name != "schema")
        fail(source_name, line_number, "unknown top-level key '" + key_name + "'");
      if (schema_seen)
        fail(source_name, line_number, "duplicate schema");
      if (value != "1")
        fail(source_name, line_number, "schema must be 1, got " + value);
      schema_seen = true;
      continue;
    }
    const std::size_t index = input_index(key_name);
    if (index == input_count)
      fail(source_name, line_number, "unknown input '" + key_name + "'");
    if (replaced[index])
      fail(source_name, line_number, "duplicate binding for " + key_name);
    replaced[index] = true;
    std::vector<Binding> bindings;
    for (const auto &source_text : parse_source_list(value, source_name, line_number)) {
      const Binding binding = parse_source(source_text, inputs[index], source_name, line_number);
      for (const auto &existing : bindings)
        if (existing.kind == binding.kind && existing.button_mask == binding.button_mask &&
            existing.source_index == binding.source_index &&
            existing.virtual_key == binding.virtual_key)
          fail(source_name, line_number, "duplicate source for " + key_name);
      bindings.push_back(binding);
    }
    map.bindings[index] = std::move(bindings);
  }
  if (!schema_seen)
    fail(source_name, line_number, "missing schema = 1");
  return map;
}

ControlMap load_controls_file(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw ConfigError(path + ": cannot open controls file");
  std::ostringstream contents;
  contents << file.rdbuf();
  return parse_controls(contents.str(), path);
}

ControlMap load_controls_from_environment() {
  const char *path = std::getenv("GT4_CONTROLS_FILE");
  if (!path || !*path)
    return default_controls();
  return load_controls_file(path);
}

std::vector<KeyboardBinding> keyboard_bindings(const ControlMap &map) {
  std::vector<KeyboardBinding> result;
  for (std::size_t index = 0; index < input_count; ++index) {
    if (inputs[index].axis)
      continue;
    for (const auto &binding : map.bindings[index])
      if (binding.kind == SourceKind::keyboard)
        result.push_back({binding.virtual_key, static_cast<std::uint16_t>(1u << inputs[index].index)});
  }
  return result;
}

TriggerReading read_trigger(const Binding &binding, std::uint8_t raw) {
  const int deadzone = deadzone_raw(binding, trigger_full);
  if (raw <= deadzone)
    return {};
  const int range = trigger_full - deadzone;
  const int distance = raw - deadzone;
  int pressure = 0;
  if (is_default_shape(binding) && binding.pressure_full == trigger_full) {
    pressure = distance * trigger_full / range;
  } else {
    const double fraction = std::min(1.0, static_cast<double>(distance) / range);
    const double shaped = std::min(1.0, shape(fraction, binding.curve) * binding.scale);
    if (binding.pressure_full == trigger_full) {
      pressure = static_cast<int>(std::floor(shaped * trigger_full));
    } else {
      // TASK-147: to the nearest of the game's steps, so a trigger a hair short of its end still gives the game's 100%.
      pressure = static_cast<int>(std::lround(shaped * binding.pressure_full));
    }
  }
  return {true, static_cast<std::uint8_t>(std::clamp(pressure, 0, trigger_full))};
}

std::uint8_t read_axis(const Binding &binding, std::int16_t raw) {
  const int value = raw;
  const int magnitude = value < 0 ? -value : value;
  const int deadzone = deadzone_raw(binding, stick_positive_full);
  if (magnitude <= deadzone)
    return 128;
  // The travel range follows the sign of the raw value, as the slice 157 mapping does.
  const int full = value < 0 ? stick_negative_full : stick_positive_full;
  const int range = full - deadzone;
  const int distance = magnitude - deadzone;
  const bool negative = (value < 0) != binding.invert;
  int steps = 0;
  if (is_default_shape(binding)) {
    steps = negative ? distance * 128 / range : distance * 127 / range;
  } else {
    const double fraction = std::min(1.0, static_cast<double>(distance) / range);
    const double shaped = std::min(1.0, shape(fraction, binding.curve) * binding.scale);
    steps = static_cast<int>(std::floor(shaped * (negative ? 128.0 : 127.0)));
  }
  if (binding.skip_game_deadzone)
    return game_stick_byte(steps, negative);
  return static_cast<std::uint8_t>(negative ? 128 - steps : 128 + steps);
}

} // namespace gt4::controls
