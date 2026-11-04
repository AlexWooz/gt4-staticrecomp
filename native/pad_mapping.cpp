#include "pad_mapping.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace gt4::pad {
namespace {

using gt4::controls::Binding;
using gt4::controls::ControlMap;
using gt4::controls::SourceKind;
using gt4::controls::input_count;
using gt4::controls::inputs;

void press(PadState &pad, std::uint8_t bit, std::uint8_t pressure) {
  pad.buttons |= static_cast<std::uint16_t>(1u << bit);
  pad.pressures[bit] = std::max(pad.pressures[bit], pressure);
}

} // namespace

PadState map_reading(const GamepadReading &reading, const ControlMap &map) {
  PadState pad;
  const std::uint8_t triggers[] = {reading.left_trigger, reading.right_trigger};
  // PS2 analog byte order: RX, RY, LX, LY.
  const std::int16_t sticks[] = {reading.thumb_rx, reading.thumb_ry, reading.thumb_lx, reading.thumb_ly};
  for (std::size_t index = 0; index < input_count; ++index) {
    const auto &input = inputs[index];
    for (const Binding &binding : map.bindings[index]) {
      if (input.axis) {
        if (binding.kind != SourceKind::xinput_axis)
          continue;
        const std::uint8_t value = gt4::controls::read_axis(binding, sticks[binding.source_index]);
        // Several bindings on one stick: the reading furthest from rest wins.
        const int deflection = std::abs(static_cast<int>(value) - 128);
        if (deflection > std::abs(static_cast<int>(pad.axes[input.index]) - 128))
          pad.axes[input.index] = value;
      } else if (binding.kind == SourceKind::xinput_button) {
        if (reading.buttons & binding.button_mask)
          press(pad, input.index, 255);
      } else if (binding.kind == SourceKind::xinput_trigger) {
        const auto pressure = gt4::controls::read_trigger(binding, triggers[binding.source_index]);
        if (pressure.pressed)
          press(pad, input.index, pressure.pressure);
      }
      // Keyboard bindings arrive as a button mask through merge_keyboard (host_io.cpp).
    }
  }
  return pad;
}

RumbleMotors rumble_motors(std::uint16_t rumble) {
  return {static_cast<std::uint16_t>((rumble >> 8) * 257),
          static_cast<std::uint16_t>((rumble & 1) ? 65535 : 0)};
}

Backend parse_backend(const char *value) {
  if (!value || !*value)
    return Backend::sdl;
  const std::string text(value);
  if (text == "sdl")
    return Backend::sdl;
  if (text == "xinput")
    return Backend::xinput;
  throw std::invalid_argument("GT4_INPUT must be sdl or xinput, got '" + text + "'");
}

const char *backend_name(Backend backend) {
  return backend == Backend::sdl ? "sdl" : "xinput";
}

std::int16_t negate_axis(std::int16_t value) {
  return value == -32768 ? static_cast<std::int16_t>(32767) : static_cast<std::int16_t>(-value);
}

std::uint8_t trigger_byte(std::int32_t sdl_value) {
  const std::int32_t clamped = std::clamp<std::int32_t>(sdl_value, 0, 32767);
  return static_cast<std::uint8_t>(clamped >> 7);
}

bool first_pad(const std::uint32_t *instances, std::size_t count, std::uint32_t &instance) {
  if (!instances || count == 0)
    return false;
  instance = *std::min_element(instances, instances + count);
  return true;
}

} // namespace gt4::pad
