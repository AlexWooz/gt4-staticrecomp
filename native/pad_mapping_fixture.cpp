// CTest fixture for the pad mapping (pad_mapping.cpp, TASK-075). Pure functions only: the SDL3 host calls them with
// the values SDL reports, so the mapping is checked here without a gamepad, a window or SDL itself.

#include "pad_mapping.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace gt4::controls;
using namespace gt4::pad;

namespace {

void require(bool value, const std::string &label) {
  if (!value)
    throw std::runtime_error(label);
}

GamepadReading at_rest() { return {}; }

// The pressure of L2 (index 8) and R2 (index 9) and the digital bits.
void check_default_buttons() {
  const ControlMap &map = default_controls();
  GamepadReading reading = at_rest();
  reading.buttons = xinput::a; // XInput A = SDL SOUTH = Cross
  PadState pad = map_reading(reading, map);
  require(pad.buttons == (1u << 14), "A is Cross (bit 14)");
  require(pad.pressures[14] == 255, "digital button has full pressure");

  reading = at_rest();
  reading.buttons = xinput::b | xinput::x | xinput::y | xinput::left_shoulder | xinput::right_shoulder;
  pad = map_reading(reading, map);
  const std::uint16_t expected = (1u << 13) | (1u << 15) | (1u << 12) | (1u << 10) | (1u << 11);
  require(pad.buttons == expected, "B, X, Y, LB, RB are Circle, Square, Triangle, L1, R1");

  reading = at_rest();
  reading.buttons = xinput::back | xinput::start | xinput::left_thumb | xinput::right_thumb;
  pad = map_reading(reading, map);
  require(pad.buttons == ((1u << 0) | (1u << 3) | (1u << 1) | (1u << 2)),
          "Back, Start and stick clicks are Select, Start, L3, R3");

  reading = at_rest();
  reading.buttons = xinput::dpad_up | xinput::dpad_down | xinput::dpad_left | xinput::dpad_right;
  pad = map_reading(reading, map);
  require(pad.buttons == ((1u << 4) | (1u << 6) | (1u << 7) | (1u << 5)), "D-pad bits");
  std::fprintf(stderr, "PASS: default button mapping (SOUTH/EAST/WEST/NORTH, shoulders, Back, Start, sticks, D-pad)\n");
}

void check_triggers_and_pressure() {
  const ControlMap &map = default_controls();
  // The 30/255 threshold: at 30 no press, at 31 a press with pressure 1, at 255 full pressure.
  for (int raw = 0; raw <= 255; ++raw) {
    GamepadReading reading = at_rest();
    reading.left_trigger = static_cast<std::uint8_t>(raw);
    const PadState pad = map_reading(reading, map);
    const bool pressed = (pad.buttons & (1u << 8)) != 0;
    require(pressed == (raw > 30), "L2 pressed exactly above the 30/255 threshold");
    const TriggerReading expected = read_trigger(map.bindings[10][0] /* l2: table position 10 */,
                                                 static_cast<std::uint8_t>(raw));
    require(pad.pressures[8] == expected.pressure, "L2 pressure equals the controls.cpp reader");
  }
  // TASK-147: R2 (throttle) spans GT4's 0..45 and L2 (brake) 0..84 over the trigger travel.
  GamepadReading reading = at_rest();
  reading.right_trigger = 255;
  reading.left_trigger = 255;
  require(map_reading(reading, map).pressures[9] == 45, "full R2 is the game's 100% throttle, pressure 45");
  require(map_reading(reading, map).pressures[8] == 84, "full L2 is the game's 100% brake, pressure 84");
  reading = at_rest();
  reading.right_trigger = 31;
  require((map_reading(reading, map).buttons & (1u << 9)) != 0 && map_reading(reading, map).pressures[9] == 0,
          "R2 just above threshold is pressed with the lowest pressure");
  std::fprintf(stderr, "PASS: trigger threshold 30/255 and analog L2/R2 pressure for all 256 values\n");
}

void check_sticks_and_dead_zones() {
  const ControlMap &map = default_controls();
  GamepadReading reading = at_rest();
  PadState pad = map_reading(reading, map);
  for (int axis = 0; axis < 4; ++axis)
    require(pad.axes[axis] == 128, "sticks at rest read 128");

  // Inside the left stick dead zone (7849 of 32767) the reading stays at rest.
  reading.thumb_lx = 7000;
  require(map_reading(reading, map).axes[2] == 128, "left stick inside its dead zone reads 128");

  // Full right on the left stick: LX positive is right (PS2 128..255).
  reading = at_rest();
  reading.thumb_lx = 32767;
  require(map_reading(reading, map).axes[2] == 255, "left stick full right is 255");
  reading.thumb_lx = -32768;
  require(map_reading(reading, map).axes[2] == 0, "left stick full left is 0");

  // Y: XInput positive is up; the default lstick_y and rstick_y invert it, so up reads below 128 (PS2 up is 0).
  reading = at_rest();
  reading.thumb_ly = 32767;
  require(map_reading(reading, map).axes[3] < 128, "left stick up reads below 128");
  reading = at_rest();
  reading.thumb_ry = -32768;
  require(map_reading(reading, map).axes[1] > 127, "right stick down reads above 128");
  std::fprintf(stderr, "PASS: stick dead zones and directions\n");
}

void check_sdl_conversions() {
  // SDL reports Y positive down; the host negates it to the XInput convention before mapping.
  require(negate_axis(32767) == -32767, "negate_axis of a positive value");
  require(negate_axis(-32768) == 32767, "negate_axis of -32768 saturates to 32767");
  require(negate_axis(0) == 0, "negate_axis of zero");
  require(trigger_byte(0) == 0, "trigger 0 is byte 0");
  require(trigger_byte(-7) == 0, "a negative trigger reading clamps to 0");
  require(trigger_byte(32767) == 255, "full trigger is byte 255");
  require(trigger_byte(16384) == 128, "half trigger is byte 128");
  require(trigger_byte(40000) == 255, "an out-of-range trigger clamps to 255");
  std::fprintf(stderr, "PASS: SDL axis and trigger conversions\n");
}

void check_remap() {
  // A remap file: Cross on B, Square on X with a quadratic R2 curve. The same mapping code serves SDL3 and XInput.
  const ControlMap remap = parse_controls(
      "schema = 1\n[bindings]\ncross = [\"xinput:B\"]\ncircle = []\nr2 = [\"xinput:RT | curve=quadratic deadzone=0.2\"]\n"
      "square = []\n",
      "remap-test");
  GamepadReading reading = at_rest();
  reading.buttons = xinput::b;
  PadState pad = map_reading(reading, remap);
  require(pad.buttons == (1u << 14), "remapped Cross is on B");
  reading = at_rest();
  reading.buttons = xinput::a;
  require(map_reading(reading, remap).buttons == 0, "A no longer drives Cross after the remap");
  reading = at_rest();
  reading.buttons = xinput::x;
  require(map_reading(reading, remap).buttons == 0, "an unbound Square stays unbound");

  // Quadratic curve with deadzone 0.2 on RT: 128 is halfway past the deadzone (about 0.5 of travel): pressure ~64.
  reading = at_rest();
  reading.right_trigger = 128;
  const std::uint8_t quadratic = map_reading(reading, remap).pressures[9];
  require(quadratic > 0 && quadratic < 128, "quadratic R2 curve gives a lower pressure than linear");
  reading.right_trigger = 255;
  require(map_reading(reading, remap).pressures[9] == 45, "full RT is R2's full range on the curve too");

  // A deadzone on the stick remap: a stick inside 0.5 of travel reads at rest.
  const ControlMap stick_map = parse_controls(
      "schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | deadzone=0.5\"]\n", "stick-test");
  reading = at_rest();
  reading.thumb_lx = 15000;
  require(map_reading(reading, stick_map).axes[2] == 128, "a 0.5 dead zone holds a stick at rest");
  std::fprintf(stderr, "PASS: controls.toml remap, quadratic trigger curve, stick dead zone\n");
}

void check_rumble() {
  RumbleMotors none = rumble_motors(0);
  require(none.low_frequency == 0 && none.high_frequency == 0, "no rumble stops both motors");
  RumbleMotors left = rumble_motors(0x0100);
  require(left.low_frequency == 257 && left.high_frequency == 0, "high byte 1 is the left motor 257");
  RumbleMotors both = rumble_motors(0xFF01);
  require(both.low_frequency == 65535 && both.high_frequency == 65535, "full rumble is 65535 on both");
  RumbleMotors right = rumble_motors(1);
  require(right.low_frequency == 0 && right.high_frequency == 65535, "low bit is the right motor");
  std::fprintf(stderr, "PASS: rumble word to motor values\n");
}

void check_backend_and_players() {
  require(parse_backend(nullptr) == Backend::sdl, "unset GT4_INPUT selects sdl");
  require(parse_backend("") == Backend::sdl, "empty GT4_INPUT selects sdl");
  require(parse_backend("sdl") == Backend::sdl, "sdl selects sdl");
  require(parse_backend("xinput") == Backend::xinput, "xinput selects the XInput path");
  bool refused = false;
  try {
    parse_backend("XInput");
  } catch (const std::invalid_argument &error) {
    refused = std::string(error.what()).find("GT4_INPUT") != std::string::npos;
  }
  require(refused, "an unknown GT4_INPUT value is refused");
  require(std::string(backend_name(Backend::sdl)) == "sdl", "backend name");

  const std::uint32_t pads[] = {12, 3, 9};
  std::uint32_t player_one = 0;
  require(first_pad(pads, 3, player_one) && player_one == 3, "the lowest instance id is player 1");
  require(!first_pad(pads, 0, player_one), "no pad means no player 1");
  std::fprintf(stderr, "PASS: GT4_INPUT selection and first pad as player 1\n");
}

} // namespace

int main() {
  try {
    check_default_buttons();
    check_triggers_and_pressure();
    check_sticks_and_dead_zones();
    check_sdl_conversions();
    check_remap();
    check_rumble();
    check_backend_and_players();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
