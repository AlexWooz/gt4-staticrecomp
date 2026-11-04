#pragma once
// Pure pad mapping (TASK-075): one gamepad reading, in the XInput convention, through the controls table to the PS2
// pad state. Shared by the XInput path and the SDL3 path (host_io.cpp), so both obey the same bindings, dead zones,
// trigger pressure curve and stick directions. No Windows or SDL dependency: pad_mapping_fixture runs on any host.

#include "controls.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::pad {

// One reading of one gamepad. Buttons use the gt4::controls::xinput::* bits. Triggers are 0..255. Sticks are
// -32768..32767 with the XInput sign convention: a positive left or right Y is up (the SDL3 host negates its Y).
struct GamepadReading {
  std::uint16_t buttons = 0;
  std::uint8_t left_trigger = 0;
  std::uint8_t right_trigger = 0;
  std::int16_t thumb_rx = 0;
  std::int16_t thumb_ry = 0;
  std::int16_t thumb_lx = 0;
  std::int16_t thumb_ly = 0;
};

// The PS2 pad state, as set_pad_buttons / set_pad_axis / set_pad_pressure take it.
struct PadState {
  std::uint16_t buttons = 0;
  std::array<std::uint8_t, 4> axes{128, 128, 128, 128};
  std::array<std::uint8_t, 16> pressures{};
};

// Maps one reading through the bindings. Several bindings on one input: buttons OR together, the highest pressure
// wins; on a stick the reading furthest from rest wins.
PadState map_reading(const GamepadReading &reading, const gt4::controls::ControlMap &map);

// Motor values (unchanged from the XInput path): the high byte of the rumble word scaled to 0..65535 on the left
// motor, and its low bit as full strength on the right motor. XInput and SDL3 both take 0..65535 values.
struct RumbleMotors {
  std::uint16_t low_frequency = 0;
  std::uint16_t high_frequency = 0;
};
RumbleMotors rumble_motors(std::uint16_t rumble);

// GT4_INPUT: "sdl" (default) reads gamepads through SDL3; "xinput" keeps the previous XInput path for one release.
enum class Backend { sdl, xinput };
// Unset or empty selects sdl. Any other value throws std::invalid_argument naming the value.
Backend parse_backend(const char *value);
const char *backend_name(Backend backend);

// Negates an SDL/XInput stick axis (SDL: down is positive; XInput: up is positive). -32768 maps to 32767.
std::int16_t negate_axis(std::int16_t value);
// SDL trigger axis (0..32767) to the XInput byte (0..255).
std::uint8_t trigger_byte(std::int32_t sdl_value);

// The pad that player 1 uses: the lowest instance id among the connected pads. Returns false when none is connected;
// otherwise `instance` receives that id.
bool first_pad(const std::uint32_t *instances, std::size_t count, std::uint32_t &instance);

// TASK-148: the controller the player chose in the launcher (GT4_PAD, the launcher's `controller` setting): `<guid>` or
// `<guid>/<serial>`, the GUID SDL3 gives the model (32 hex digits) and the serial number when the pad reports one.
struct PadChoice {
  std::string guid;    // lowercase
  std::string serial;  // empty when the pad reports none
};
// False for unset, empty, "auto" or malformed text: then controller 1 is read.
bool parse_pad_choice(const char *value, PadChoice &choice);

struct PadCandidate {
  std::uint32_t instance = 0;
  std::string guid;
  std::string serial;
};
// The pad to read: the chosen one (same GUID and serial), else the first pad of the chosen model, else controller 1 (the
// lowest instance id). False with no pad. The launcher applies the same rule (gt4l-platform pad::chosen_pad).
bool choose_pad(const std::vector<PadCandidate> &pads, const PadChoice *choice, std::uint32_t &instance);

} // namespace gt4::pad
