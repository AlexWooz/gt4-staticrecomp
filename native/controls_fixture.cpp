// CTest fixture for the input binding table (controls.cpp). No Windows input or
// audio is touched: the readers are pure functions over raw XInput values.

#include "controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace gt4::controls;

namespace {

void require(bool value, const std::string &label) {
  if (!value)
    throw std::runtime_error(label);
}

bool same_binding(const Binding &a, const Binding &b) {
  return a.kind == b.kind && a.button_mask == b.button_mask &&
         a.source_index == b.source_index && a.virtual_key == b.virtual_key &&
         a.deadzone == b.deadzone && a.scale == b.scale && a.invert == b.invert &&
         a.curve == b.curve;
}

bool same_map(const ControlMap &a, const ControlMap &b) {
  for (std::size_t index = 0; index < input_count; ++index) {
    const auto &left = a.bindings[index];
    const auto &right = b.bindings[index];
    if (left.size() != right.size())
      return false;
    for (std::size_t item = 0; item < left.size(); ++item)
      if (!same_binding(left[item], right[item]))
        return false;
  }
  return true;
}

std::size_t input_index_of(const std::string &name) {
  for (std::size_t index = 0; index < input_count; ++index)
    if (name == inputs[index].name)
      return index;
  throw std::runtime_error("fixture: unknown input " + name);
}

// The slice 157 mapping, copied from the previous host_io.cpp. The new readers must
// reproduce these values bit for bit.
struct LegacyTrigger {
  bool pressed;
  std::uint8_t pressure;
};

LegacyTrigger legacy_trigger(std::uint8_t raw) {
  if (raw <= 30)
    return {false, 0};
  return {true, static_cast<std::uint8_t>((raw - 30) * 255 / (255 - 30))};
}

std::uint8_t legacy_axis(std::int16_t raw, int deadzone, bool invert) {
  const int value = raw;
  const int magnitude = value < 0 ? -value : value;
  if (magnitude <= deadzone)
    return 128;
  const int range = (value < 0 ? 32768 : 32767) - deadzone;
  const int distance = magnitude - deadzone;
  const bool negative = (value < 0) != invert;
  return static_cast<std::uint8_t>(negative ? 128 - distance * 128 / range
                                            : 128 + distance * 127 / range);
}

ControlMap parse_text(const std::string &text) { return parse_controls(text, "fixture.toml"); }

// First binding of an input. Callers that need an exact count check it themselves.
const Binding &only_binding(const ControlMap &map, const std::string &name) {
  const auto &bindings = map.bindings[input_index_of(name)];
  require(!bindings.empty(), "fixture: expected a binding for " + name);
  return bindings.front();
}

void expect_config_error(const std::string &text, const std::string &fragment, int line,
                         const std::string &label) {
  try {
    parse_text(text);
  } catch (const ConfigError &error) {
    const std::string message = error.what();
    const std::string prefix = "fixture.toml:" + std::to_string(line) + ": ";
    require(message.find(prefix) == 0 && message.find(fragment) != std::string::npos,
            label + ": got '" + message + "'");
    return;
  }
  throw std::runtime_error(label + ": accepted an invalid file");
}

void check_default_keyboard_table() {
  // The TASK-076 keyboard layout (docs/CONTROLS.md, keyboard-default), as (VK, PS2 bit). Several keys per button.
  const std::pair<std::uint8_t, std::uint16_t> layout[] = {
      {0x26, 1u << 4},  {'E', 1u << 4},       // D-pad up: Up, E
      {0x27, 1u << 5},  {'D', 1u << 5},       // D-pad right: Right, D
      {0x28, 1u << 6},                        // D-pad down: Down
      {0x25, 1u << 7},  {'A', 1u << 7},       // D-pad left: Left, A
      {'W', 1u << 14},  {0x0D, 1u << 14},     // Cross: W, Enter
      {0x20, 1u << 13},                       // Circle (handbrake): Space
      {'S', 1u << 15},                        // Square (brake): S
      {0x08, 1u << 12}, {0x1B, 1u << 12},     // Triangle (back): Backspace, Esc
      {'Q', 1u << 10},                        // L1 (look back): Q
      {0x10, 1u << 11},                       // R1 (nitro): Shift
      {'F', 1u << 8},                         // L2 (gear down): F
      {'R', 1u << 9},                         // R2 (gear up): R
      {'C', 1u << 0}, {0x09, 1u << 0},        // Select (camera): C, Tab
      {'P', 1u << 3}};                        // Start: P (hypothesis, see docs/CONTROLS.md)
  const auto keys = keyboard_bindings(default_controls());
  require(keys.size() == 20, "default keyboard table has 20 keys");
  for (const auto &expected : layout) {
    bool found = false;
    for (const auto &key : keys)
      found = found || (key.virtual_key == expected.first && key.button_mask == expected.second);
    require(found, "default keyboard key present");
  }
  std::fprintf(stderr, "PASS: default keyboard table is the TASK-076 layout\n");
}

void check_defaults_and_example_files() {
  const std::string directory = GT4_CONTROLS_EXAMPLES_DIR;
  const ControlMap from_file = load_controls_file(directory + "/default.toml");
  require(same_map(from_file, default_controls()),
          "default.toml equals the built-in defaults");
  const ControlMap pedals = load_controls_file(directory + "/triggers-as-pedals.toml");
  require(only_binding(pedals, "l2").kind == SourceKind::xinput_trigger &&
              only_binding(pedals, "r2").kind == SourceKind::xinput_trigger,
          "pedals example binds triggers to L2 and R2");
  require(pedals.bindings[input_index_of("cross")].size() == 3 &&
              pedals.bindings[input_index_of("cross")][0].button_mask == 0x1000,
          "pedals example keeps the default cross binding (XInput A, keys W and Enter)");
  const ControlMap keyboard = load_controls_file(directory + "/keyboard-only.toml");
  require(keyboard.bindings[input_index_of("lstick_x")].empty() &&
              keyboard.bindings[input_index_of("rstick_y")].empty(),
          "keyboard-only example leaves sticks unbound");
  require(keyboard_bindings(keyboard).size() == 16, "keyboard-only example binds 16 keys");
  std::fprintf(stderr, "PASS: default.toml equals defaults; example files parse\n");
}

void check_trigger_readings() {
  // With the whole byte (range=255) a trigger reads as slice 157 did.
  const Binding left = only_binding(parse_text("schema = 1\n[bindings]\nl2 = [\"xinput:LT | range=255\"]\n"), "l2");
  std::uint8_t previous = 0;
  for (int raw = 0; raw <= 255; ++raw) {
    const auto reading = read_trigger(left, static_cast<std::uint8_t>(raw));
    const auto expected = legacy_trigger(static_cast<std::uint8_t>(raw));
    require(reading.pressed == expected.pressed && reading.pressure == expected.pressure,
            "a range=255 trigger reading equals slice 157 at every raw value");
    require(reading.pressure >= previous, "trigger pressure is monotone");
    previous = reading.pressure;
  }
  require(!read_trigger(left, 30).pressed, "trigger deadzone: 30 is at rest");
  require(read_trigger(left, 31).pressure == 1, "trigger first step is pressure 1");
  require(read_trigger(left, 255).pressure == 255, "trigger full travel is pressure 255");

  // TASK-147: the default L2 (brake) spans 0..84 and the default R2 (throttle) 0..45 over the travel past the deadzone,
  // GT4's own 100% (min(100 * min(2p, 256) / d, 256), d = 65 brake, 35 throttle). The digital bit is as before.
  const struct { const char *input; std::uint8_t full; int game_divisor; } pedals[] = {{"l2", 84, 65}, {"r2", 45, 35}};
  for (const auto &pedal : pedals) {
    const Binding &binding = only_binding(default_controls(), pedal.input);
    require(binding.pressure_full == pedal.full, "the default trigger range is the game's 100%");
    std::uint8_t prior = 0;
    for (int raw = 0; raw <= 255; ++raw) {
      const auto reading = read_trigger(binding, static_cast<std::uint8_t>(raw));
      require(reading.pressed == legacy_trigger(static_cast<std::uint8_t>(raw)).pressed, "the digital bit is unchanged");
      const long expected = raw <= 30 ? 0 : std::lround((raw - 30) * static_cast<double>(pedal.full) / 225.0);
      require(reading.pressure == expected && reading.pressure >= prior, "the range spreads evenly over the travel");
      prior = reading.pressure;
      // What the game makes of it: below its 100% until the last few percent of the travel.
      const int game = std::min(100 * std::min(2 * static_cast<int>(reading.pressure), 256) / pedal.game_divisor, 256);
      if (raw < 248) require(game < 256, "the game's 100% comes only at the end of the travel");
    }
    require(read_trigger(binding, 255).pressure == pedal.full, "full travel is the game's 100%");
    // Half the travel past the deadzone is about half of the game's value (128 of 256).
    const int half = read_trigger(binding, 30 + 113).pressure;
    const int game_half = 100 * 2 * half / pedal.game_divisor;
    require(game_half >= 118 && game_half <= 138, "half the travel is about half throttle or brake in the game");
  }
  const Binding custom = only_binding(parse_text("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=90\"]\n"), "r2");
  require(read_trigger(custom, 255).pressure == 90, "range sets the pressure at full travel");
  const Binding on_face = only_binding(parse_text("schema = 1\n[bindings]\ncross = [\"xinput:RT\"]\n"), "cross");
  require(read_trigger(on_face, 255).pressure == 255, "a trigger on another button keeps the whole byte");

  // Deadzone 0.5 (raw 128 of 255): rest up to 128, then 255 / 127 per unit.
  const Binding half = only_binding(parse_text("schema = 1\n[bindings]\nl2 = [\"xinput:LT | deadzone=0.5 range=255\"]\n"), "l2");
  require(!read_trigger(half, 128).pressed && read_trigger(half, 129).pressure == 2,
          "custom trigger deadzone sets the rest point");

  // Quadratic: half travel gives about a quarter of full pressure.
  const Binding quadratic = only_binding(
      parse_text("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=quadratic range=255\"]\n"), "l2");
  const auto middle = read_trigger(quadratic, 142).pressure;
  require(middle >= 62 && middle <= 64, "quadratic curve: half travel near quarter pressure");

  // S-Curve, smooth power, sublinear and logarithmic curves
  const Binding scurve = only_binding(
      parse_text("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=s_curve range=255\"]\n"), "l2");
  const auto scurve_mid = read_trigger(scurve, 142).pressure;
  require(scurve_mid >= 125 && scurve_mid <= 128, "s-curve at half travel is near half pressure");

  const Binding sublinear = only_binding(
      parse_text("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=sublinear range=255\"]\n"), "l2");
  const auto sublinear_mid = read_trigger(sublinear, 142).pressure;
  require(sublinear_mid >= 175 && sublinear_mid <= 185, "sublinear curve at half travel is near 70% pressure");

  // Exact byte vectors also used by the browser preview checks: raw 128, zero deadzone.
  const struct { const char *name; std::uint8_t middle; } curves[] = {
      {"linear", 128}, {"quadratic", 64}, {"cubic", 32}, {"s_curve", 128},
      {"smooth_power", 90}, {"sublinear", 180}, {"logarithmic", 189}};
  for (const auto &curve : curves) {
    const auto controls = parse_text(std::string("schema = 1\n[bindings]\nl2 = [\"xinput:LT | range=255 deadzone=0 curve=") +
                                    curve.name + "\"]\n");
    const auto &binding = only_binding(controls, "l2");
    require(read_trigger(binding, 0).pressure == 0, "every curve rests at zero");
    require(read_trigger(binding, 128).pressure == curve.middle, "quantized curve middle vector");
    require(read_trigger(binding, 255).pressure == 255, "every curve reaches full pressure");
    std::uint8_t prior = 0;
    for (int raw = 0; raw <= 255; ++raw) {
      const auto pressure = read_trigger(binding, static_cast<std::uint8_t>(raw)).pressure;
      require(pressure >= prior, "all curve pressure bytes are monotone");
      prior = pressure;
    }
  }

  // Default deadzone (30/255): endpoints, representative middle vector at raw 142, and monotonicity.
  const struct { const char *name; std::uint8_t middle; } default_dz_curves[] = {
      {"linear", 126}, {"quadratic", 63}, {"cubic", 31}, {"s_curve", 126},
      {"smooth_power", 89}, {"sublinear", 179}, {"logarithmic", 188}};
  for (const auto &curve : default_dz_curves) {
    const auto controls = parse_text(std::string("schema = 1\n[bindings]\nl2 = [\"xinput:LT | range=255 curve=") +
                                    curve.name + "\"]\n");
    const auto &binding = only_binding(controls, "l2");
    require(read_trigger(binding, 0).pressure == 0, "every default-deadzone curve rests at zero");
    require(read_trigger(binding, 30).pressure == 0, "every default-deadzone curve rests at threshold");
    require(read_trigger(binding, 142).pressure == curve.middle, "quantized default-deadzone curve middle vector");
    require(read_trigger(binding, 255).pressure == 255, "every default-deadzone curve reaches full pressure");
    std::uint8_t prior = 0;
    for (int raw = 0; raw <= 255; ++raw) {
      const auto pressure = read_trigger(binding, static_cast<std::uint8_t>(raw)).pressure;
      require(pressure >= prior, "default-deadzone curve pressure bytes are monotone");
      prior = pressure;
    }
  }

  // Representative half deadzone (0.5): endpoints, middle vector at raw 192, and monotonicity.
  const struct { const char *name; std::uint8_t middle; } half_dz_curves[] = {
      {"linear", 128}, {"quadratic", 64}, {"cubic", 32}, {"s_curve", 129},
      {"smooth_power", 91}, {"sublinear", 181}, {"logarithmic", 189}};
  for (const auto &curve : half_dz_curves) {
    const auto controls = parse_text(std::string("schema = 1\n[bindings]\nl2 = [\"xinput:LT | range=255 deadzone=0.5 curve=") +
                                    curve.name + "\"]\n");
    const auto &binding = only_binding(controls, "l2");
    require(read_trigger(binding, 0).pressure == 0, "half-deadzone curve rests at zero");
    require(read_trigger(binding, 128).pressure == 0, "half-deadzone curve rests at threshold");
    require(read_trigger(binding, 192).pressure == curve.middle, "quantized half-deadzone curve middle vector");
    require(read_trigger(binding, 255).pressure == 255, "half-deadzone curve reaches full pressure");
    std::uint8_t prior = 0;
    for (int raw = 0; raw <= 255; ++raw) {
      const auto pressure = read_trigger(binding, static_cast<std::uint8_t>(raw)).pressure;
      require(pressure >= prior, "half-deadzone curve pressure bytes are monotone");
      prior = pressure;
    }
  }

  // Scale 0.5 halves full travel; scale 2 is clamped at full pressure.
  const Binding half_scale = only_binding(
      parse_text("schema = 1\n[bindings]\nr2 = [\"xinput:RT | scale=0.5 range=255\"]\n"), "r2");
  require(read_trigger(half_scale, 255).pressure == 127, "scale 0.5 halves full travel");
  const Binding double_scale = only_binding(
      parse_text("schema = 1\n[bindings]\nr2 = [\"xinput:RT | scale=2 range=255\"]\n"), "r2");
  require(read_trigger(double_scale, 255).pressure == 255, "scale 2 clamps at full pressure");
  std::fprintf(stderr, "PASS: trigger readings: slice 157 match, deadzone, curve, scale\n");
}

// TASK-147: what GT4's remapper makes of a stick byte (knots 0, 90, 165, 255 to 0, 128, 128, 255; integer division).
int game_stick_out(int byte) {
  if (byte <= 90)
    return (128 * byte + 45) / 90;
  if (byte < 165)
    return 128;
  return 128 + (127 * (byte - 165) + 45) / 90;
}

void check_axis_readings() {
  const auto &map = default_controls();
  // With the XInput dead zones and game_deadzone=keep a stick reads as slice 157 did.
  const struct {
    const char *name;
    const char *source;
    const char *deadzone_text;
    int deadzone;
    bool invert;
  } sticks[] = {{"rstick_x", "RX", "0.26517533", 8689, false}, {"rstick_y", "RY", "0.26517533", 8689, true},
                {"lstick_x", "LX", "0.23953978", 7849, false}, {"lstick_y", "LY", "0.23953978", 7849, true}};
  for (const auto &stick : sticks) {
    const std::string text = std::string("schema = 1\n[bindings]\n") + stick.name + " = [\"xinput:" + stick.source +
                             " | deadzone=" + stick.deadzone_text + " game_deadzone=keep" +
                             (stick.invert ? " invert=true" : "") + "\"]\n";
    const Binding legacy = only_binding(parse_text(text), stick.name);
    for (int raw = -32768; raw <= 32767; ++raw) {
      const auto value = static_cast<std::int16_t>(raw);
      require(read_axis(legacy, value) == legacy_axis(value, stick.deadzone, stick.invert),
              std::string("XInput dead zone with game_deadzone=keep equals slice 157: ") + stick.name);
    }
  }
  // TASK-147: the defaults: a 10% dead zone (3277 of 32767), and the byte steps over GT4's own dead zone, so the game's
  // steering is exactly this binding's linear reading, from its first step past the dead zone to full lock.
  for (const auto &stick : sticks) {
    const Binding &binding = only_binding(map, stick.name);
    require(binding.skip_game_deadzone, "sticks skip the game's dead zone by default");
    int first_step_raw = 0;
    int prior_game = game_stick_out(read_axis(binding, -32768)) - 128;
    for (int raw = -32768; raw <= 32767; ++raw) {
      const auto value = static_cast<std::int16_t>(raw);
      const int linear = static_cast<int>(legacy_axis(value, 3277, stick.invert)) - 128;
      const int game = game_stick_out(read_axis(binding, value)) - 128;
      // The game has 90 bytes for 127 steps past its band: within one step of the linear reading, never dead.
      require(std::abs(game - linear) <= 1 && (linear == 0) == (game == 0),
              std::string("the game sees the binding's linear reading, to the nearest step: ") + stick.name);
      require(stick.invert ? game <= prior_game : game >= prior_game, std::string("the game's steering is monotone: ") + stick.name);
      prior_game = game;
      if (raw > 0 && linear != 0 && first_step_raw == 0)
        first_step_raw = raw;
    }
    require(first_step_raw > 3277 && first_step_raw < 3277 + 400, "the game moves from the first step past 10%");
  }
  // Without the compensation the game's dead zone stacked: about 46% of the travel did nothing (owner, TASK-147).
  const Binding keep = only_binding(parse_text("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | deadzone=0.23953978 game_deadzone=keep\"]\n"), "lstick_x");
  require(game_stick_out(read_axis(keep, static_cast<std::int16_t>(32767 * 45 / 100))) == 128, "the old mapping: dead at 45%");
  require(game_stick_out(read_axis(only_binding(map, "lstick_x"), static_cast<std::int16_t>(32767 * 45 / 100))) >= 128 + 48,
          "the new mapping: 45% of the travel steers about 39%");
  // A custom deadzone and an inverted sign keep the rest value at 128 inside the zone.
  const ControlMap custom = parse_text(
      "schema = 1\n[bindings]\nlstick_y = [\"xinput:LY | deadzone=0.5 invert=true\"]\n");
  const Binding &custom_y = only_binding(custom, "lstick_y");
  require(read_axis(custom_y, 16000) == 128 && read_axis(custom_y, 0) == 128,
          "custom stick deadzone holds the rest value");
  require(read_axis(custom_y, 32767) < 128, "inverted stick: full up reads low");
  std::fprintf(stderr, "PASS: stick readings: slice 157 with game_deadzone=keep, linear game steering by default\n");
}

void check_stick_curves_and_signed_endpoints() {
  const char *curve_names[] = {
      "linear", "quadratic", "cubic", "s_curve", "smooth_power", "sublinear", "logarithmic"
  };
  for (const char *curve_name : curve_names) {
    for (bool invert : {false, true}) {
      const std::string text = std::string("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | deadzone=0.2 curve=") +
                               curve_name + " invert=" + (invert ? "true" : "false") + "\"]\n";
      const ControlMap map = parse_text(text);
      const Binding &binding = only_binding(map, "lstick_x");

      // 1. Neutral (raw = 0) is always 128 (PS2 stick center)
      require(read_axis(binding, 0) == 128, std::string(curve_name) + " stick neutral is 128");

      // 2. Deadzone: deadzone = 0.2 of 32767 is 6553.
      // Values within [-6553, 6553] must yield exactly 128.
      require(read_axis(binding, 6553) == 128, std::string(curve_name) + " stick deadzone upper boundary");
      require(read_axis(binding, -6553) == 128, std::string(curve_name) + " stick deadzone lower boundary");
      if (!invert) {
        require(read_axis(binding, 6554) >= 128, std::string(curve_name) + " stick positive step past deadzone");
        require(read_axis(binding, -6554) <= 128, std::string(curve_name) + " stick negative step past deadzone");
      } else {
        require(read_axis(binding, 6554) <= 128, std::string(curve_name) + " stick inverted positive step");
        require(read_axis(binding, -6554) >= 128, std::string(curve_name) + " stick inverted negative step");
      }

      // 3. Signed endpoints:
      // Positive full (32767): non-inverted -> 255, inverted -> 0
      // Negative full (-32768): non-inverted -> 0, inverted -> 255
      if (!invert) {
        require(read_axis(binding, 32767) == 255, std::string(curve_name) + " stick pos endpoint 255");
        require(read_axis(binding, -32768) == 0, std::string(curve_name) + " stick neg endpoint 0");
      } else {
        require(read_axis(binding, 32767) == 0, std::string(curve_name) + " stick pos inverted endpoint 0");
        require(read_axis(binding, -32768) == 255, std::string(curve_name) + " stick neg inverted endpoint 255");
      }

      // 4. Monotonicity across all 65536 raw values from -32768 to 32767
      std::uint8_t prior = read_axis(binding, -32768);
      for (int raw = -32768; raw <= 32767; ++raw) {
        std::uint8_t current = read_axis(binding, static_cast<std::int16_t>(raw));
        if (!invert) {
          require(current >= prior, std::string(curve_name) + " non-inverted stick monotonic");
        } else {
          require(current <= prior, std::string(curve_name) + " inverted stick monotonic");
        }
        prior = current;
      }
    }
  }
  std::fprintf(stderr, "PASS: stick curves: neutral 128, signed endpoints, deadzones, monotonicity\n");
}

void dump_curve_vectors(const std::string &path) {
  const struct { const char *name; } curves[] = {
      {"linear"}, {"quadratic"}, {"cubic"}, {"s_curve"},
      {"smooth_power"}, {"sublinear"}, {"logarithmic"}};
  const double deadzones[] = {0.0, 30.0 / 255.0, 0.25, 0.5, 0.75};

  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw std::runtime_error("cannot open vector output path: " + path);

  out << "{\n";
  out << "  \"curves\": [\"linear\", \"quadratic\", \"cubic\", \"s_curve\", \"smooth_power\", \"sublinear\", \"logarithmic\"],\n";
  out << "  \"deadzones\": [0.0, 0.11764705882352941, 0.25, 0.5, 0.75],\n";
  out << "  \"vectors\": {\n";

  bool first_entry = true;
  for (std::size_t c = 0; c < 7; ++c) {
    for (std::size_t d = 0; d < 5; ++d) {
      if (!first_entry) out << ",\n";
      first_entry = false;

      std::ostringstream conf;
      conf << "schema = 1\n[bindings]\nl2 = [\"xinput:LT | range=255 deadzone=" << deadzones[d]
           << " curve=" << curves[c].name << "\"]\n";
      const ControlMap map = parse_text(conf.str());
      const Binding &b = only_binding(map, "l2");

      out << "    \"" << curves[c].name << "_dz" << d << "\": [";
      for (int raw = 0; raw <= 255; ++raw) {
        if (raw > 0) out << ",";
        const auto reading = read_trigger(b, static_cast<std::uint8_t>(raw));
        out << static_cast<int>(reading.pressure);
      }
      out << "]";
    }
  }
  out << "\n  }\n}\n";
  std::fprintf(stderr, "Wrote vector dump to %s\n", path.c_str());
}


void check_remaps() {
  const ControlMap remap = parse_text(
      "schema = 1\n[bindings]\ncross = [\"key:J\"]\nsquare = [\"xinput:X\", \"key:H\"]\n"
      "lstick_x = []\n");
  const auto keys = keyboard_bindings(remap);
  bool found_j = false;
  bool found_z = false;
  for (const auto &key : keys) {
    found_j = found_j || (key.virtual_key == 'J' && key.button_mask == (1u << 14));
    found_z = found_z || key.virtual_key == 'Z';
  }
  require(found_j && !found_z, "keyboard remap replaces the cross key");
  require(only_binding(remap, "cross").kind == SourceKind::keyboard,
          "remapped cross has no XInput A binding");
  require(remap.bindings[input_index_of("square")].size() == 2,
          "two sources on one input");
  require(remap.bindings[input_index_of("lstick_x")].empty(), "empty list unbinds an input");
  require(remap.bindings[input_index_of("circle")].size() == 2 &&
              only_binding(remap, "circle").kind == SourceKind::xinput_button,
          "unlisted inputs keep their defaults");
  std::fprintf(stderr, "PASS: keyboard and XInput remaps; unbinding; defaults kept\n");
}

void check_invalid_files() {
  expect_config_error("", "missing schema = 1", 1, "empty file");
  expect_config_error("schema = 2\n", "schema must be 1", 1, "wrong schema");
  expect_config_error("schema = 1\n[pad]\n", "unknown table [pad]", 2, "unknown table");
  expect_config_error("schema = 1\n[bindings]\ntriangel = [\"key:S\"]\n", "unknown input 'triangel'",
                      3, "misspelled input");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"xinput:Q\"]\n", "unknown XInput source",
                      3, "unknown XInput name");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"key:Nope\"]\n", "unknown key", 3,
                      "unknown key name");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"key:F8\"]\n", "reserved", 3,
                      "host shortcut key");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:A\"]\n", "is a stick axis", 3,
                      "button bound to an axis");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"xinput:LX\"]\n",
                      "a stick cannot drive it", 3, "stick bound to a button");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"key:Z | deadzone=0.1\"]\n",
                      "digital sources take no options", 3, "options on a key");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | deadzone=1\"]\n",
                      "deadzone must be", 3, "deadzone of one");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | deadzone=abc\"]\n",
                      "not a number", 3, "deadzone not a number");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | scale=3\"]\n", "scale must be",
                      3, "scale above two");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=exp\"]\n",
                      "curve must be", 3, "unknown curve");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | mode=dualsense\"]\n",
                      "unknown option 'mode'", 3, "unimplemented raw trigger mode on trigger");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | mode=dualsense\"]\n",
                      "unknown option 'mode'", 3, "unimplemented raw trigger mode on stick");
  expect_config_error("schema = 1\n[bindings]\nl2 = [\"xinput:LT | invert=true\"]\n",
                      "invert applies to stick axes", 3, "invert on a trigger");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | invert=yes\"]\n",
                      "invert must be true or false", 3, "invert not boolean");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | gain=2\"]\n",
                      "unknown option 'gain'", 3, "unknown option");
  expect_config_error("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=0\"]\n", "range must be", 3, "range of zero");
  expect_config_error("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=256\"]\n", "range must be", 3, "range over a byte");
  expect_config_error("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=4.5\"]\n", "range must be", 3, "range not whole");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | range=45\"]\n", "range applies to triggers only", 3,
                      "range on a stick");
  expect_config_error("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | game_deadzone=off\"]\n", "game_deadzone must be", 3,
                      "game_deadzone value");
  expect_config_error("schema = 1\n[bindings]\nr2 = [\"xinput:RT | game_deadzone=keep\"]\n", "game_deadzone applies to stick axes only", 3,
                      "game_deadzone on a trigger");
  expect_config_error("schema = 1\n[bindings]\ncross = xinput:A\n",
                      "quoted string or an array", 3, "unquoted value");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"xinput:A\" \"key:Z\"]\n",
                      "expected ',' between", 3, "missing comma");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"xinput:A\"\n",
                      "quoted string or an array", 3, "unterminated array");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"xinput:A\"]\ncross = [\"key:Z\"]\n",
                      "duplicate binding for cross", 4, "duplicate input");
  expect_config_error("schema = 1\n[bindings]\ncross = [\"key:Z\", \"key:Z\"]\n",
                      "duplicate source", 3, "duplicate source");
  expect_config_error("schema = 1\nfoo = 2\n", "unknown top-level key", 2, "top-level key");
  expect_config_error("schema = 1\n[bindings]\nno_equals_here\n", "expected name = value", 3,
                      "line without equals");
  bool missing_rejected = false;
  try {
    load_controls_file("no-such-controls-file.toml");
  } catch (const ConfigError &error) {
    missing_rejected = std::string(error.what()).find("cannot open") != std::string::npos;
  }
  require(missing_rejected, "missing file is a clear error");
  std::fprintf(stderr, "PASS: 24 invalid inputs rejected with file and line\n");
}

void check_environment_selection() {
  // Unset and empty select the defaults.
  _putenv_s("GT4_CONTROLS_FILE", "");
  require(same_map(load_controls_from_environment(), default_controls()),
          "empty GT4_CONTROLS_FILE selects defaults");
  const std::string path = "controls_fixture_env.toml";
  {
    std::ofstream file(path, std::ios::binary);
    file << "schema = 1\n[bindings]\ncross = [\"key:J\"]\n";
  }
  _putenv_s("GT4_CONTROLS_FILE", path.c_str());
  require(only_binding(load_controls_from_environment(), "cross").kind == SourceKind::keyboard,
          "GT4_CONTROLS_FILE loads the named file");
  {
    std::ofstream file(path, std::ios::binary);
    file << "schema = 1\n[bindings]\ncross = [\"xinput:Nope\"]\n";
  }
  bool rejected = false;
  try {
    load_controls_from_environment();
  } catch (const ConfigError &error) {
    rejected = std::string(error.what()).find(path + ":3:") == 0;
  }
  require(rejected, "invalid GT4_CONTROLS_FILE is refused with its path and line");
  std::remove(path.c_str());
  _putenv_s("GT4_CONTROLS_FILE", "");
  std::fprintf(stderr, "PASS: GT4_CONTROLS_FILE selection and refusal\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "--dump-vectors") {
    try {
      dump_curve_vectors(argv[2]);
      return 0;
    } catch (const std::exception &error) {
      std::fprintf(stderr, "FAIL: %s\n", error.what());
      return 1;
    }
  }
  try {
    check_default_keyboard_table();
    check_defaults_and_example_files();
    check_trigger_readings();
    check_axis_readings();
    check_stick_curves_and_signed_endpoints();
    check_remaps();
    check_invalid_files();
    check_environment_selection();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
