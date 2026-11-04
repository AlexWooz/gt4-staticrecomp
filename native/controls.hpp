#pragma once
// Input bindings for the PS2 pad: logical inputs, host sources, the strict
// TOML-subset reader (docs/CONTROLS.md) and the pure value readers used by
// host_io.cpp. This header has no Windows dependency so controls_fixture can
// test the mapping on any host.

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gt4::controls {

// XInput button bits, equal to XINPUT_GAMEPAD_* (host_io.cpp static_asserts them).
namespace xinput {
inline constexpr std::uint16_t dpad_up = 0x0001;
inline constexpr std::uint16_t dpad_down = 0x0002;
inline constexpr std::uint16_t dpad_left = 0x0004;
inline constexpr std::uint16_t dpad_right = 0x0008;
inline constexpr std::uint16_t start = 0x0010;
inline constexpr std::uint16_t back = 0x0020;
inline constexpr std::uint16_t left_thumb = 0x0040;
inline constexpr std::uint16_t right_thumb = 0x0080;
inline constexpr std::uint16_t left_shoulder = 0x0100;
inline constexpr std::uint16_t right_shoulder = 0x0200;
inline constexpr std::uint16_t a = 0x1000;
inline constexpr std::uint16_t b = 0x2000;
inline constexpr std::uint16_t x = 0x4000;
inline constexpr std::uint16_t y = 0x8000;
} // namespace xinput

// A logical PS2 input. Digital inputs use `index` as their PS2 button bit, which is
// also their pressure index. Axes use `index` as their PS2 analog byte slot in the
// order RX, RY, LX, LY (PS2 bytes 5..8).
struct InputInfo {
  const char *name;
  bool axis;
  std::uint8_t index;
};

inline constexpr std::size_t input_count = 20;

// Order fixes the bindings table layout; names are the config keys.
inline constexpr std::array<InputInfo, input_count> inputs{{
    {"dpad_up", false, 4},    {"dpad_right", false, 5}, {"dpad_down", false, 6},
    {"dpad_left", false, 7},  {"cross", false, 14},     {"circle", false, 13},
    {"square", false, 15},    {"triangle", false, 12},  {"l1", false, 10},
    {"r1", false, 11},        {"l2", false, 8},         {"r2", false, 9},
    {"l3", false, 1},         {"r3", false, 2},         {"select", false, 0},
    {"start", false, 3},      {"rstick_x", true, 0},    {"rstick_y", true, 1},
    {"lstick_x", true, 2},    {"lstick_y", true, 3},
}};

enum class SourceKind : std::uint8_t {
  xinput_button, // digital XInput button; full pressure
  xinput_trigger, // XInput trigger 0..255; digital above deadzone, pressure from the value
  xinput_axis, // XInput stick axis -32768..32767; only drives axis inputs
  keyboard, // Win32 virtual key; digital, full pressure
};

enum class Curve : std::uint8_t {
  linear,
  quadratic,
  cubic,
  s_curve,
  smooth_power,
  sublinear,
  logarithmic,
};

struct Binding {
  SourceKind kind = SourceKind::keyboard;
  std::uint16_t button_mask = 0; // xinput_button: xinput::* bit
  std::uint8_t source_index = 0; // trigger: 0 left, 1 right; axis: PS2 slot 0..3 (RX, RY, LX, LY)
  std::uint8_t virtual_key = 0; // keyboard: VK_* code
  double deadzone = 0.0; // fraction of full travel, 0 <= deadzone < 1
  double scale = 1.0; // 0 <= scale <= 2, applied after the curve, clamped to full travel
  bool invert = false; // axes only
  Curve curve = Curve::linear;
  // Triggers: the pressure byte at full travel (option `range`). GT4 takes a much smaller byte than 255 as 100% throttle
  // or brake, so R2 and L2 default to the game's own 100% (controls.cpp, TASK-147); other inputs to 255.
  std::uint8_t pressure_full = 255;
  // TASK-161: the same with the game's full trigger range on (GT4_TRIGGER_FULL_RANGE=1), where 255 is the game's 100% for
  // the throttle and the brake (option `range_full`, default 255 for every trigger). `range` applies to the game's own
  // scale only and `range_full` to the full one, so a range made for one scale never caps the other.
  std::uint8_t pressure_full_wide = 255;
  // Sticks: step over GT4's own stick dead zone (option `game_deadzone=skip|keep`, TASK-147), so the game turns from the
  // first step past this binding's dead zone and linearly to full lock.
  bool skip_game_deadzone = true;
};

struct ControlMap {
  // One vector per entry of `inputs`, in the same order.
  std::array<std::vector<Binding>, input_count> bindings;
};

struct KeyboardBinding {
  std::uint8_t virtual_key = 0;
  std::uint16_t button_mask = 0; // one PS2 button bit
};

// Reported by every parser and loader. what() is "<source>:<line>: <message>".
class ConfigError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Built-in bindings: the XInput map and the keyboard map of slice 157 and earlier.
const ControlMap &default_controls();

// Parses the TOML subset described in docs/CONTROLS.md. Inputs listed in the file
// replace their defaults; unlisted inputs keep them; `[]` unbinds an input.
ControlMap parse_controls(std::string_view text, const std::string &source_name);

// Reads and parses a file; throws ConfigError when it cannot be read.
ControlMap load_controls_file(const std::string &path);

// GT4_CONTROLS_FILE: unset or empty selects the defaults, otherwise the file is loaded.
ControlMap load_controls_from_environment();

// Digital keyboard bindings, used by window.cpp for GetAsyncKeyState polling.
std::vector<KeyboardBinding> keyboard_bindings(const ControlMap &map);

struct TriggerReading {
  bool pressed = false;
  std::uint8_t pressure = 0;
};

// TASK-161: which pressure scale the game uses (native/trigger_range.hpp): its own (stock, the default) or the full range.
// The game executable sets it once at start from GT4_TRIGGER_FULL_RANGE, before any pad is read; fixtures pass it.
enum class PressureScale { stock, full };
void set_pressure_scale(PressureScale scale);
PressureScale pressure_scale();
// The pressure byte at full travel for a binding under a scale: pressure_full (stock) or pressure_full_wide (full).
std::uint8_t pressure_full_for(const Binding &binding, PressureScale scale);

// Reads an XInput trigger (0..255) through one binding, under the scale the game uses (pressure_scale()).
TriggerReading read_trigger(const Binding &binding, std::uint8_t raw);
// The same under a given scale.
TriggerReading read_trigger(const Binding &binding, std::uint8_t raw, PressureScale scale);

// Reads an XInput stick axis (-32768..32767) through one binding; returns the PS2
// byte, 128 at rest.
std::uint8_t read_axis(const Binding &binding, std::int16_t raw);

} // namespace gt4::controls
