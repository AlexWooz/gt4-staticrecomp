#pragma once
// Racing wheel for the native Driving Force Pro path (TASK-018 by WaimateWihongi in PR #205; TASK-181).
//
// wheel.toml names the host wheel and maps its steering, pedals and buttons onto the controls of the virtual Logitech
// Driving Force Pro (DFP) on the emulated USB bus (native/usb_dfp.hpp). GT4 runs its own wheel driver on that device and
// sends its own force feedback, which native/dfp_haptics.hpp plays on the host wheel. This is the only wheel path: the
// 1.2.x bridge onto the DualShock 2 pad was removed in TASK-181, and `mode = "dfp"` is the default.
//
// The file is found through GT4_WHEEL_FILE (the launcher's Wheel page sets it at Play and writes it in game.ini), else
// beside GT4_CONTROLS_FILE, else beside the game executable. A file the parser refuses is logged and the game starts
// without the wheel. Any SDL3 joystick can be the wheel; separate pedal, handbrake or shifter devices are bound with
// "dev:<guid>:" sources.

#include "controls.hpp"
#include "dfp_haptics.hpp"
#include "usb_dfp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Joystick;

namespace gt4::wheel {

using Curve = gt4::controls::Curve;
using ConfigError = gt4::controls::ConfigError;

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
  // The host wheel's physical lock-to-lock range (180..1080). It maps 1:1 onto the range the game sets on the DFP
  // (GT4 sets 900 degrees), so it must equal the range set in the wheel's own software.
  int rotation_degrees = 900;
  bool invert = false;
};

struct PedalConfig {
  SourceRef source{};
  PedalAxisMode mode = PedalAxisMode::inverted;
  double deadzone = 0.02;   // 0.0..0.45
  double saturation = 0.98; // 0.20..1.0
  Curve curve = Curve::linear;
};

inline constexpr std::size_t button_slot_count = 16;

struct ButtonSlotInfo {
  const char *name;  // the key in wheel.toml [buttons]
  const char *label; // the DFP control it feeds
  int dfp_bit;       // bit in the DFP's 14-bit button field; -1 for the D-pad (the DFP's hat)
};

// The [buttons] keys keep the DualShock 2 names of the 1.2.x files, so those files keep their meaning: the shift slots
// r2/l2 (right/left paddle, gear up/down) feed the DFP's R1/L1, and r1/l1 feed the DFP's R2/L2. Button bits follow
// docs/research/DFP-PCSX2-FACTS.md: 0 cross, 1 square, 2 circle, 3 triangle, 4 R1, 5 L1, 6 R2, 7 L2, 8 select, 9 start,
// 10 R3, 11 L3.
inline constexpr std::array<ButtonSlotInfo, button_slot_count> button_slots{{
    {"dpad_up", "D-pad up", -1},
    {"dpad_right", "D-pad right", -1},
    {"dpad_down", "D-pad down", -1},
    {"dpad_left", "D-pad left", -1},
    {"cross", "Cross", 0},
    {"circle", "Circle", 2},
    {"square", "Square", 1},
    {"triangle", "Triangle", 3},
    {"l1", "DFP L2", 7},
    {"r1", "DFP R2", 6},
    {"l2", "Gear down (DFP L1, left paddle)", 5},
    {"r2", "Gear up (DFP R1, right paddle)", 4},
    {"l3", "L3", 11},
    {"r3", "R3", 10},
    {"select", "Select", 8},
    {"start", "Start", 9},
}};

struct WheelConfig {
  int version = 1;
  bool enabled = false;
  std::string device_guid; // empty = auto-detect primary wheel
  std::string device_name;
  std::string preset; // the launcher's template or preset name; informational
  SteeringConfig steering{};
  PedalConfig throttle{};
  PedalConfig brake{};
  PedalConfig handbrake{}; // presses the DFP's Circle past half travel
  PedalConfig clutch{};    // the DFP has no clutch: parsed for the launcher's live view, never sent to the game
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
  std::uint32_t instance_id = 0;  // stable identity through filtered connected-device snapshots
};

// Generic bindings (steering axis 0, throttle 1, brake 2, clutch 3, buttons 0..11, hat 0); enabled = false.
const WheelConfig &default_wheel_config();

// Parses and formats source strings ("axis:0", "btn:4", "hat:0:up", "dev:<guid>:axis:1").
SourceRef parse_source_ref(std::string_view text, const std::string &source_name, std::size_t line_no);
std::string format_source_ref(const SourceRef &ref);

const char *pedal_axis_mode_name(PedalAxisMode mode);
PedalAxisMode parse_pedal_axis_mode(std::string_view text, const std::string &source_name, std::size_t line_no);
const char *curve_name(Curve curve);

// Strict parser for wheel.toml (the launcher validates with the same rules before it saves). Keys of the removed pad
// bridge (pedal_map_mode, effective_degrees, steering deadzone/linearity/sensitivity, centering_spring, curb_rumble) are
// accepted and ignored, so 1.2.x files still load; mode = "pad" is refused.
WheelConfig parse_wheel_config(std::string_view text, const std::string &source_name);
std::string serialize_wheel_config(const WheelConfig &config);
WheelConfig load_wheel_file(const std::string &path);

struct WheelFileResult {
  WheelConfig config;     // enabled = false when no file was found or it was refused
  std::string path;       // the file that was read, empty when none was found
  std::string error;      // why the file was refused; the game then runs without the wheel
};
WheelFileResult load_wheel_from_environment();

// Pure pedal reader: raw signed 16-bit axis (-32768..32767) or button state to normalized 0.0..1.0 and the byte 0..255.
double normalize_pedal_axis(PedalAxisMode mode, std::int16_t raw);
std::uint8_t read_pedal(const PedalConfig &config, std::int16_t raw_axis, bool digital_pressed = false);

// VID:PID from an SDL GUID string (bytes 4-5 and 8-9, little-endian). False for other GUID layouts.
bool extract_guid_vid_pid(std::string_view guid, std::uint16_t &vid, std::uint16_t &pid);
// Wheel bases by USB VID:PID (docs/research/WHEEL-AUTOMAP-TABLE.md), including models SDL's own wheel list lacks
// (Turtle Beach VelocityOne Race, Simagic EVO, PXN V10/V12). A vendor alone is never enough.
bool is_known_wheel_vid_pid(std::uint16_t vid, std::uint16_t pid);

// Whether a device is a primary racing wheel candidate when device_guid is empty: SDL's wheel type, a known wheel
// VID:PID, or a wheel model name; never a device named as pedals, a shifter or a handbrake.
bool is_candidate_wheel_device(bool sdl_is_wheel_type, bool sdl_is_gamepad, std::string_view device_name,
                               std::string_view guid = "");

// Selects the primary device index from `devices` according to `config.device_guid`.
// Returns -1 when no matching wheel device is present.
int select_primary_device(const WheelConfig &config, const std::vector<DeviceReading> &devices);

// The DFP's input for a device snapshot. Steering: the host wheel's physical angle (rotation_degrees across the full
// axis) on the range the game set (`game_range_degrees`, 200 or 900), 14 bits, 0 = full left, 0x1FFF centre, 0x3FFF
// full right. Pedals 0..255 pressed. Buttons and the hat from [buttons]; the handbrake presses Circle.
gt4::usb::DfpInput map_dfp_input(const WheelConfig &config, const std::vector<DeviceReading> &devices, int primary,
                                 int game_range_degrees);

// Runtime: SDL3 joysticks, the virtual DFP's input at every VBlank, and the game's forces on the wheel.
class WheelInput {
public:
  // `haptics`: how forces reach the wheel. Empty = SDL3 haptics opened from the wheel's joystick. `threaded`: the haptic
  // device is owned by a small thread (default; GT4_WHEEL_HAPTIC_THREAD=0 applies forces on the emulation thread).
  explicit WheelInput(WheelConfig config = default_wheel_config(), HapticSinkFactory haptics = {},
                      bool threaded = haptic_thread_wanted());
  ~WheelInput();
  WheelInput(const WheelInput &) = delete;
  WheelInput &operator=(const WheelInput &) = delete;

  // Every VBlank: reads the devices, chooses the primary wheel, updates the DFP's input (neutral without focus) and starts
  // or stops the forces with the focus.
  void prepare(bool focused);
  // Focus loss: neutral input and the forces stopped (once, however often it is called).
  void stop();
  const std::vector<std::uint32_t> &owned_device_ids() const { return owned_device_ids_; }
  std::uint32_t primary_instance_id() const;

  bool enabled() const { return config_.enabled; }
  bool has_active_wheel() const { return primary_joystick_ != nullptr; }
  const WheelConfig &config() const { return config_; }
  DfpHapticThread::Stats haptic_stats() const { return haptics_.stats(); }
  // One line for the log and the launcher's session check: whether the game's forces reached the wheel.
  std::string force_summary() const;
  void wait_haptics_idle() { haptics_.wait_idle(); }

  static bool haptic_thread_wanted();

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
  void set_primary(SDL_Joystick *joystick, bool focused);
  bool references_device(const std::string &guid) const;

  WheelConfig config_;
  std::vector<std::uint32_t> owned_device_ids_;
  bool sdl_initialized_ = false;
  bool haptic_initialized_ = false;
  bool haptic_attached_ = false;
  std::uint64_t next_scan_ms_ = 0;
  std::vector<OpenDevice> devices_;
  SDL_Joystick *primary_joystick_ = nullptr;
  DfpHapticThread haptics_;
};

} // namespace gt4::wheel
