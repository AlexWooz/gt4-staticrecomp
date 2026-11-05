#pragma once
// Universal racing wheel configuration, pure axis/pedal/button mapping, and SDL3
// joystick + haptics runtime (TASK-018).
//
// Reads %LOCALAPPDATA%\GT4Recomp\wheel.toml when GT4_WHEEL_FILE points to it.
// Supports any SDL3 Joystick / HID racing wheel, multi-USB setups (separate
// wheelbase, USB pedals, handbrake, H-shifter via "dev:<guid>:..." sources),
// configurable steering lock (180..1080 degrees), pedal polarity modes
// (inverted, normal, positive, combined_negative, combined_positive), and
// SDL3 Haptics / rumble Force Feedback.

#include "controls.hpp"
#include "pad_mapping.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Joystick;
struct SDL_Haptic;

namespace gt4::wheel {

using Curve = gt4::controls::Curve;
using ConfigError = gt4::controls::ConfigError;
using PadState = gt4::pad::PadState;

// How analog throttle and brake pedals drive the PS2 DualShock 2 pad.
enum class PedalMapMode : std::uint8_t {
  cross_square, // Throttle -> Cross (pressure) + rstick_y Up; Brake -> Square (pressure) + rstick_y Down
  rstick_y,     // Throttle -> rstick_y Up; Brake -> rstick_y Down
  l2_r2,        // Throttle -> R2 (pressure); Brake -> L2 (pressure)
  both,         // Throttle -> Cross + R2 (pressure); Brake -> Square + L2 (pressure)
};

// How a raw signed 16-bit joystick axis (-32768..32767) maps to 0.0..1.0 pedal travel.
enum class PedalAxisMode : std::uint8_t {
  inverted,          // +32767 at rest -> -32768 at full press (standard HID/Logitech/Thrustmaster pedals)
  normal,            // -32768 at rest -> +32767 at full press (Direct Drive / load-cell controllers)
  positive,          // 0 at rest -> +32767 at full press (unipolar HID axes)
  combined_negative, // 0 at rest -> -32768 at full press (shared gas/brake axis, negative half)
  combined_positive, // 0 at rest -> +32767 at full press (shared gas/brake axis, positive half)
};

// Hat switch direction for D-pad bindings on wheels.
enum class HatDirection : std::uint8_t {
  up,
  right,
  down,
  left,
};

enum class SourceKind : std::uint8_t {
  none,
  axis,          // "axis:N" or "dev:<guid>:axis:N"
  axis_positive, // "axis:N+" or "dev:<guid>:axis:N+" (button threshold > +0.5)
  axis_negative, // "axis:N-" or "dev:<guid>:axis:N-" (button threshold < -0.5)
  button,        // "btn:N" or "dev:<guid>:btn:N"
  hat,           // "hat:N:up|right|down|left" or "dev:<guid>:hat:N:..."
};

struct SourceRef {
  SourceKind kind = SourceKind::none;
  std::string device_guid; // empty = primary wheel device; 32 lowercase hex chars = specific USB device
  std::uint8_t index = 0;
  HatDirection hat_dir = HatDirection::up;

  bool empty() const { return kind == SourceKind::none; }
  bool operator==(const SourceRef &other) const = default;
};

struct SteeringConfig {
  SourceRef source{SourceKind::axis, {}, 0, HatDirection::up};
  int rotation_degrees = 900;  // physical wheel lock-to-lock range (180..1080)
  int effective_degrees = 900; // effective lock range mapped to full PS2 steering (180..1080)
  double deadzone = 0.0;       // 0.0..0.45
  Curve linearity = Curve::linear;
  double sensitivity = 1.0;    // 0.25..2.5
  bool invert = false;
};

struct PedalConfig {
  SourceRef source{};
  PedalAxisMode mode = PedalAxisMode::inverted;
  double deadzone = 0.02;   // 0.0..0.45
  double saturation = 0.98; // 0.20..1.0
  Curve curve = Curve::linear;
};

struct FfbConfig {
  bool enabled = true;
  double strength = 1.0;          // 0.0..2.0
  double centering_spring = 0.25; // 0.0..1.0
  double damper = 0.15;           // 0.0..1.0 (DFP mode: the least damping the wheel always has)
  double curb_rumble = 1.0;       // 0.0..2.0
  bool invert = false;
};

inline constexpr std::size_t button_slot_count = 16;

struct ButtonSlotInfo {
  const char *name;
  const char *wheel_label;
  std::uint8_t ps2_bit;
};

// The 16 digital PS2 pad buttons in canonical order.
inline constexpr std::array<ButtonSlotInfo, button_slot_count> button_slots{{
    {"dpad_up", "D-Pad Up", 4},
    {"dpad_right", "D-Pad Right", 5},
    {"dpad_down", "D-Pad Down", 6},
    {"dpad_left", "D-Pad Left", 7},
    {"cross", "Confirm / Accelerate (Cross)", 14},
    {"circle", "Handbrake / Cancel (Circle)", 13},
    {"square", "Brake (Square)", 15},
    {"triangle", "Reverse / Back (Triangle)", 12},
    {"l1", "Look Back (L1)", 10},
    {"r1", "Nitro / High Beam (R1)", 11},
    {"l2", "Shift Down (L2)", 8},
    {"r2", "Shift Up (R2)", 9},
    {"l3", "Horn / L3", 1},
    {"r3", "R3", 2},
    {"select", "Camera / View (Select)", 0},
    {"start", "Pause / Start (Start)", 3},
}};

struct WheelConfig {
  int version = 1;
  bool enabled = false;
  std::string device_guid; // empty = auto-detect primary wheel
  std::string device_name;
  std::string preset = "generic";
  // TASK-018: "pad" maps the wheel onto the DualShock 2 (the 1.2.x bridge); "dfp" feeds the virtual Driving Force Pro
  // on the emulated USB bus (native/usb_dfp.hpp) and plays the game's own force feedback on the wheel.
  bool dfp = false;
  PedalMapMode pedal_map_mode = PedalMapMode::cross_square;
  SteeringConfig steering{};
  PedalConfig throttle{};
  PedalConfig brake{};
  PedalConfig handbrake{};
  PedalConfig clutch{};
  FfbConfig ffb{};
  std::array<std::vector<SourceRef>, button_slot_count> buttons{};
};

// Snapshot of one connected SDL joystick/wheel device for pure evaluation.
struct DeviceReading {
  std::string guid;
  std::string name;
  bool is_wheel = false;
  bool is_gamepad = false;
  std::vector<std::int16_t> axes;
  std::vector<bool> buttons;
  std::vector<std::uint8_t> hats; // SDL_HAT_* bitmask (0x01 up, 0x02 right, 0x04 down, 0x08 left)
  std::uint32_t instance_id = 0; // stable identity through filtered connected-device snapshots
};

// Built-in presets: "generic", "hori", "logitech_g29", "logitech_dfgt", "thrustmaster",
// "fanatec_moza", "combined_pedals".
WheelConfig preset_wheel_config(std::string_view preset_name);
const WheelConfig &default_wheel_config();

// Parses and formats source strings ("axis:0", "btn:4", "hat:0:up", "dev:<guid>:axis:1").
SourceRef parse_source_ref(std::string_view text, const std::string &source_name, std::size_t line_no);
std::string format_source_ref(const SourceRef &ref);

const char *pedal_map_mode_name(PedalMapMode mode);
PedalMapMode parse_pedal_map_mode(std::string_view text, const std::string &source_name, std::size_t line_no);

const char *pedal_axis_mode_name(PedalAxisMode mode);
PedalAxisMode parse_pedal_axis_mode(std::string_view text, const std::string &source_name, std::size_t line_no);

const char *curve_name(Curve curve);

// Strict TOML parser/serializer for wheel.toml.
WheelConfig parse_wheel_config(std::string_view text, const std::string &source_name);
std::string serialize_wheel_config(const WheelConfig &config);
WheelConfig load_wheel_file(const std::string &path);

// Loads GT4_WHEEL_FILE when set and non-empty. Returns default_wheel_config() with enabled=false
// when GT4_WHEEL_FILE is unset or empty. No implicit user-settings lookup is performed.
// `has_explicit_file` is set to true when GT4_WHEEL_FILE was provided.
WheelConfig load_wheel_from_environment(bool *has_explicit_file = nullptr);

// Pure steering reader: maps raw signed 16-bit axis (-32768..32767) to PS2 lstick_x byte (0..255, 128 center).
std::uint8_t read_steering(const SteeringConfig &config, std::int16_t raw);

// Pure pedal reader: maps raw signed 16-bit axis (-32768..32767) or button state to normalized 0.0..1.0 and PS2 byte 0..255.
double normalize_pedal_axis(PedalAxisMode mode, std::int16_t raw);
std::uint8_t read_pedal(const PedalConfig &config, std::int16_t raw_axis, bool digital_pressed = false);

// Evaluates whether a device matches auto-detection as a primary racing wheel when device_guid is empty.
bool is_candidate_wheel_device(bool sdl_is_wheel_type, bool sdl_is_gamepad, std::string_view device_name,
                                std::string_view guid = "");

// Selects the primary device index from `devices` according to `config.device_guid`.
// Returns -1 when no matching wheel device is present.
int select_primary_device(const WheelConfig &config, const std::vector<DeviceReading> &devices);

// Evaluates a full multi-device snapshot into a PS2 PadState.
// Sets `active` to true if a primary wheel or bound auxiliary device contributed input.
PadState map_wheel_devices(const WheelConfig &config,
                           const std::vector<DeviceReading> &devices,
                           bool *active = nullptr);

// Merges a wheel PadState onto the base controller+keyboard PadState:
// buttons OR together, highest pressure wins, and any axis moved away from 128 overrides rest (or furthest from 128 wins).
void merge_wheel_state(PadState &base, const PadState &wheel_state);

// Computes scaled 16-bit rumble/FFB magnitudes from the PS2 rumble word and FfbConfig.
struct WheelFfbCommand {
  std::uint16_t low_frequency = 0;  // 0..65535
  std::uint16_t high_frequency = 0; // 0..65535
  float normalized_strength = 0.0f; // 0.0..1.0 for SDL_PlayHapticRumble
  int autocenter_percent = 0;       // 0..100 for SDL_SetHapticAutocenter
  int gain_percent = 0;           // 0..100 for SDL_SetHapticGain
};
WheelFfbCommand compute_ffb_command(const FfbConfig &config, std::uint16_t ps2_rumble);

// Optional command sink for backend-independent lifecycle verification. Production uses SDL handles.
struct HapticOutput {
  std::function<void(int)> gain;
  std::function<void(int)> autocenter;
  std::function<void()> stop_all;
  std::function<void()> stop_rumble;
  std::function<void(float)> rumble;
  std::function<void(std::uint16_t, std::uint16_t)> joystick_rumble;
};
void stop_haptic_output(const HapticOutput &output);

// Runtime SDL3 Joystick + Haptics manager for the game window.
class WheelInput {
public:
  explicit WheelInput(WheelConfig config = default_wheel_config(), bool explicit_config = false,
                      HapticOutput output = {});
  ~WheelInput();
  WheelInput(const WheelInput &) = delete;
  WheelInput &operator=(const WheelInput &) = delete;

  // Polls all connected SDL joysticks, maps the active wheel configuration, and merges onto `pad`.
  void poll_and_merge(bool focused, PadState &pad);
  // Prepare ownership before ControllerInput polls; merge the same snapshot afterwards.
  void prepare(bool focused);
  void merge_prepared(PadState &pad) const;
  const std::vector<std::uint32_t> &owned_device_ids() const { return owned_device_ids_; }
  std::uint32_t primary_instance_id() const;

  // Applies PS2 pad rumble / Force Feedback to the active wheel.
  void rumble(bool focused, std::uint16_t ps2_rumble);

  // Stops all active rumble/FFB effects immediately (e.g., on focus loss or exit).
  void stop();

  bool enabled() const { return config_.enabled; }
  bool explicit_config() const { return explicit_config_; }
  bool has_active_wheel() const { return primary_joystick_ != nullptr; }
  const WheelConfig &config() const { return config_; }

private:
  struct OpenDevice {
    std::uint32_t instance_id = 0;
    SDL_Joystick *joystick = nullptr;
    std::string guid;
    std::string name;
    bool is_wheel = false;
    bool is_gamepad = false;
  };

  void scan_devices();
  void close_all_devices();
  void open_haptic_for_primary(SDL_Joystick *joystick);
  void close_haptic();
  void resume_condition_effects();
  void prepare_dfp(bool focused, const std::vector<DeviceReading> &readings, int primary);
  void open_dfp_effects();
  void apply_dfp_forces(bool focused);
  HapticOutput haptic_output();

  WheelConfig config_;
  HapticOutput output_override_;
  std::vector<std::uint32_t> owned_device_ids_;
  PadState prepared_pad_{};
  bool prepared_active_ = false;
  bool haptic_suspended_ = true;
  bool haptic_open_attempted_ = false;
  bool explicit_config_ = false;
  bool sdl_initialized_ = false;
  bool haptic_initialized_ = false;
  bool rumble_initialized_ = false;
  std::uint64_t next_scan_ms_ = 0;
  std::uint64_t last_rumble_ms_ = 0;
  std::uint16_t last_rumble_raw_ = 0;
  std::vector<OpenDevice> devices_;
  SDL_Joystick *primary_joystick_ = nullptr;
  SDL_Haptic *primary_haptic_ = nullptr;
  std::uint32_t haptic_features_ = 0;
  int spring_effect_ = -1;  // DirectInput spring (centring) when the device has no autocentre
  int damper_effect_ = -1;
  // DFP mode: one persistent effect per force kind, updated in place (restarting effects upsets some wheels).
  int dfp_constant_ = -1;
  int dfp_spring_ = -1;
  int dfp_damper_ = -1;
  int dfp_friction_ = -1;
  bool dfp_running_ = false;
  bool dfp_focused_ = false;
  std::uint64_t dfp_revision_ = ~0ull;
  std::uint64_t dfp_next_log_ms_ = 0;
  std::uint64_t dfp_starts_ = 0;
  int dfp_errors_ = 0;
};

} // namespace gt4::wheel
