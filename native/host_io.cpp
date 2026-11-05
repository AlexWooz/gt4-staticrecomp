#include "host_io.hpp"
#include "perf_probe.hpp"
#include "platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>
#include <string>

using gt4::controls::ControlMap;
using gt4::controls::SourceKind;

#ifdef _WIN32
static_assert(gt4::controls::xinput::dpad_up == XINPUT_GAMEPAD_DPAD_UP);
static_assert(gt4::controls::xinput::dpad_right == XINPUT_GAMEPAD_DPAD_RIGHT);
static_assert(gt4::controls::xinput::dpad_down == XINPUT_GAMEPAD_DPAD_DOWN);
static_assert(gt4::controls::xinput::dpad_left == XINPUT_GAMEPAD_DPAD_LEFT);
static_assert(gt4::controls::xinput::start == XINPUT_GAMEPAD_START);
static_assert(gt4::controls::xinput::back == XINPUT_GAMEPAD_BACK);
static_assert(gt4::controls::xinput::left_thumb == XINPUT_GAMEPAD_LEFT_THUMB);
static_assert(gt4::controls::xinput::right_thumb == XINPUT_GAMEPAD_RIGHT_THUMB);
static_assert(gt4::controls::xinput::left_shoulder == XINPUT_GAMEPAD_LEFT_SHOULDER);
static_assert(gt4::controls::xinput::right_shoulder == XINPUT_GAMEPAD_RIGHT_SHOULDER);
static_assert(gt4::controls::xinput::a == XINPUT_GAMEPAD_A);
static_assert(gt4::controls::xinput::b == XINPUT_GAMEPAD_B);
static_assert(gt4::controls::xinput::x == XINPUT_GAMEPAD_X);
static_assert(gt4::controls::xinput::y == XINPUT_GAMEPAD_Y);
#endif

namespace {
#ifdef _WIN32
void check_audio(MMRESULT result, const char *operation) {
  if (result == MMSYSERR_NOERROR)
    return;
  char description[MAXERRORLENGTH]{};
  waveOutGetErrorTextA(result, description, MAXERRORLENGTH);
  throw std::runtime_error(std::string(operation) + ": " + description + " (" +
                           std::to_string(result) + ")");
}
#endif

void press(PadState &pad, std::uint8_t bit, std::uint8_t pressure) {
  pad.buttons |= static_cast<std::uint16_t>(1u << bit);
  pad.pressures[bit] = std::max(pad.pressures[bit], pressure);
}
} // namespace

#ifdef _WIN32
// Stick and trigger readings are computed by controls.cpp (read_axis, read_trigger), through pad_mapping.cpp.
PadState map_gamepad(const XINPUT_GAMEPAD &gamepad, const ControlMap &map) {
  gt4::pad::GamepadReading reading;
  reading.buttons = gamepad.wButtons;
  reading.left_trigger = gamepad.bLeftTrigger;
  reading.right_trigger = gamepad.bRightTrigger;
  reading.thumb_rx = gamepad.sThumbRX;
  reading.thumb_ry = gamepad.sThumbRY;
  reading.thumb_lx = gamepad.sThumbLX;
  reading.thumb_ly = gamepad.sThumbLY;
  return gt4::pad::map_reading(reading, map);
}

PadState map_gamepad(const XINPUT_GAMEPAD &gamepad) {
  return map_gamepad(gamepad, gt4::controls::default_controls());
}
#endif

void merge_keyboard(PadState &pad, std::uint16_t buttons) {
  pad.buttons |= buttons;
  for (int bit = 0; bit < 16; ++bit)
    if (buttons & (1u << bit))
      pad.pressures[bit] = 255;
}

#ifdef _WIN32
XINPUT_VIBRATION map_rumble(std::uint16_t rumble) {
  const auto motors = gt4::pad::rumble_motors(rumble);
  return {static_cast<WORD>(motors.low_frequency), static_cast<WORD>(motors.high_frequency)};
}
#endif

ControllerInput::ControllerInput(ControlMap map, gt4::pad::Backend backend)
    : map_(std::move(map)), backend_(backend) {
#ifndef _WIN32
  // TASK-180: XInput exists only on Windows; elsewhere every pad goes through SDL3.
  if (backend_ != gt4::pad::Backend::sdl) {
    std::fprintf(stderr, "[input] GT4_INPUT=%s is Windows-only; using SDL3\n", gt4::pad::backend_name(backend_));
    backend_ = gt4::pad::Backend::sdl;
  }
#endif
  if (backend_ == gt4::pad::Backend::sdl) {
    // Gamepads only: SDL creates no window and takes no part in the game's message loop.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD))
      throw std::runtime_error(std::string("SDL_Init(SDL_INIT_GAMEPAD) failed: ") + SDL_GetError());
    sdl_initialized_ = true;
    // TASK-148: the controller chosen on the launcher's Controls page. SDL itself reads SDL_GAMECONTROLLERCONFIG_FILE (the
    // player's own mappings) at SDL_Init.
    has_pad_choice_ = gt4::pad::parse_pad_choice(std::getenv("GT4_PAD"), pad_choice_);
    if (has_pad_choice_)
      std::fprintf(stderr, "[input] controller chosen in the launcher: %s%s%s\n", pad_choice_.guid.c_str(),
                   pad_choice_.serial.empty() ? "" : " serial ", pad_choice_.serial.c_str());
  }
}

ControllerInput::~ControllerInput() {
  stop();
  close_sdl_pad();
  if (sdl_initialized_)
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void ControllerInput::close_sdl_pad() {
  if (sdl_pad_) {
    SDL_CloseGamepad(sdl_pad_);
    sdl_pad_ = nullptr;
  }
}

bool ControllerInput::is_excluded(std::uint32_t instance) const {
  return std::find(excluded_devices_.begin(), excluded_devices_.end(), instance) != excluded_devices_.end();
}

void ControllerInput::set_excluded_devices(std::vector<std::uint32_t> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids == excluded_devices_)
    return;
  excluded_devices_ = std::move(ids);
  if (sdl_pad_ && is_excluded(SDL_GetGamepadID(sdl_pad_))) {
    std::fprintf(stderr, "[input] SDL gamepad is owned by the wheel; looking for another controller\n");
    stop();
    close_sdl_pad();
  }
  sdl_next_scan_ = 0;
}

PadState ControllerInput::poll(bool focused, std::uint16_t keyboard_buttons) {
#ifdef _WIN32
  PadState pad = backend_ == gt4::pad::Backend::sdl ? poll_sdl(focused) : poll_xinput(focused);
#else
  PadState pad = poll_sdl(focused);
#endif
  // TASK-180 test3 (owner's phone): some controllers reach the Activity as Android keys and motion but never appear in
  // SDL's gamepad list (no HID gamepad mapping for that pad). GameActivity forwards them here; they are the pad while
  // SDL has none, and never a second reading of a pad SDL does see.
  if (backend_ == gt4::pad::Backend::sdl && sdl_pad_ == nullptr && android_active_) {
    pad = android_pad();
    if (!android_used_) {
      android_used_ = true;
      log_devices("android key/motion pad (SDL opened no gamepad)", focused);
    }
  }
  // TASK-180 test3: the first read and every device change write the `[input]` block, after the scan above, so the
  // "which one the game reads" line is the truth of that moment. The fingerprint is a plain hash of the device list.
  const std::uint64_t fingerprint = device_fingerprint();
  if (!devices_logged_) {
    devices_logged_ = true;
    log_devices("start", focused);
  } else if (fingerprint != devices_fingerprint_) {
    log_devices("device change", focused);
  }
  devices_fingerprint_ = fingerprint;
  // Without focus the pad is neutral: the keyboard counts only in the game window.
  if (!focused)
    return pad;
  merge_keyboard(pad, keyboard_buttons);
  return pad;
}

// TASK-180 test3: the Android key and motion fallback. Key codes and axes are the Android ones (KeyEvent.KEYCODE_*,
// MotionEvent.AXIS_*), which GameActivity passes through as plain numbers. The mapping is the same standardized gamepad
// reading read_sdl_gamepad builds (face buttons by position, sticks, triggers, hat), so controls.cpp applies unchanged.
void ControllerInput::android_key(int keycode, bool down) {
  std::uint16_t mask = 0;
  switch (keycode) {
    case 96: mask = gt4::controls::xinput::a; break;             // KEYCODE_BUTTON_A
    case 97: mask = gt4::controls::xinput::b; break;             // KEYCODE_BUTTON_B
    case 99: mask = gt4::controls::xinput::x; break;             // KEYCODE_BUTTON_X
    case 100: mask = gt4::controls::xinput::y; break;            // KEYCODE_BUTTON_Y
    case 102: mask = gt4::controls::xinput::left_shoulder; break;   // KEYCODE_BUTTON_L1
    case 103: mask = gt4::controls::xinput::right_shoulder; break;  // KEYCODE_BUTTON_R1
    case 106: mask = gt4::controls::xinput::left_thumb; break;   // KEYCODE_BUTTON_THUMBL
    case 107: mask = gt4::controls::xinput::right_thumb; break;  // KEYCODE_BUTTON_THUMBR
    case 108: mask = gt4::controls::xinput::start; break;        // KEYCODE_BUTTON_START
    case 109: mask = gt4::controls::xinput::back; break;         // KEYCODE_BUTTON_SELECT
    case 19: mask = gt4::controls::xinput::dpad_up; break;       // KEYCODE_DPAD_UP
    case 20: mask = gt4::controls::xinput::dpad_down; break;     // KEYCODE_DPAD_DOWN
    case 21: mask = gt4::controls::xinput::dpad_left; break;     // KEYCODE_DPAD_LEFT
    case 22: mask = gt4::controls::xinput::dpad_right; break;    // KEYCODE_DPAD_RIGHT
    case 104:  // KEYCODE_BUTTON_L2: a digital trigger, the full travel (an analog one arrives as a motion axis)
    case 105:  // KEYCODE_BUTTON_R2
      android_active_ = true;
      if (keycode == 104)
        android_left_trigger_ = down ? 255 : 0;
      else
        android_right_trigger_ = down ? 255 : 0;
      return;
    default: return;
  }
  android_active_ = true;
  if (down)
    android_buttons_ |= mask;
  else
    android_buttons_ &= static_cast<std::uint16_t>(~mask);
}

void ControllerInput::android_motion(int axis, float value) {
  const auto to_sdl = [](float axis_value) {
    return static_cast<std::int16_t>(std::clamp(axis_value, -1.0f, 1.0f) * 32767.0f);
  };
  const auto to_trigger = [](float axis_value) {
    return static_cast<std::uint8_t>(std::clamp(axis_value, 0.0f, 1.0f) * 255.0f);
  };
  switch (axis) {
    case 0: android_axes_[0] = to_sdl(value); break;                    // AXIS_X
    case 1: android_axes_[1] = static_cast<std::int16_t>(-to_sdl(value)); break;  // AXIS_Y (Android down is positive)
    case 2: android_axes_[2] = to_sdl(value); break;                    // AXIS_Z (right stick X)
    case 11: android_axes_[3] = static_cast<std::int16_t>(-to_sdl(value)); break; // AXIS_RZ (right stick Y)
    case 17: android_left_trigger_ = to_trigger(value); break;          // AXIS_LTRIGGER
    case 18: android_right_trigger_ = to_trigger(value); break;         // AXIS_RTRIGGER
    case 23: android_left_trigger_ = to_trigger(value); break;          // AXIS_BRAKE
    case 22: android_right_trigger_ = to_trigger(value); break;         // AXIS_GAS
    case 14:                                                            // AXIS_HAT_Y: a hat, -1 up
    case 15: {                                                          // AXIS_HAT_X: -1 left
      const std::uint16_t up = gt4::controls::xinput::dpad_up, down = gt4::controls::xinput::dpad_down;
      const std::uint16_t left = gt4::controls::xinput::dpad_left, right = gt4::controls::xinput::dpad_right;
      const std::uint16_t pair = axis == 14 ? static_cast<std::uint16_t>(up | down) : static_cast<std::uint16_t>(left | right);
      android_buttons_ &= static_cast<std::uint16_t>(~pair);
      if (value < -0.5f) android_buttons_ |= axis == 14 ? up : left;
      if (value > 0.5f) android_buttons_ |= axis == 14 ? down : right;
      break;
    }
    default: return;
  }
  android_active_ = true;
}

PadState ControllerInput::android_pad() const {
  gt4::pad::GamepadReading reading;
  reading.buttons = android_buttons_;
  reading.left_trigger = android_left_trigger_;
  reading.right_trigger = android_right_trigger_;
  reading.thumb_lx = android_axes_[0];
  reading.thumb_ly = android_axes_[1];
  reading.thumb_rx = android_axes_[2];
  reading.thumb_ry = android_axes_[3];
  return gt4::pad::map_reading(reading, map_);
}

std::uint64_t ControllerInput::device_fingerprint() const {
  if (backend_ == gt4::pad::Backend::xinput)
    return 1;  // one slot, no hot-plug list; the connect and disconnect lines come from poll_xinput
  std::uint64_t hash = 1469598103934665603ull;
  int count = 0;
  SDL_JoystickID *ids = SDL_GetJoysticks(&count);
  const auto mix = [&hash](const char *text) {
    for (const unsigned char *character = reinterpret_cast<const unsigned char *>(text); character != nullptr && *character;
         ++character)
      hash = (hash ^ *character) * 1099511628211ull;
  };
  for (int index = 0; ids != nullptr && index < count; ++index) {
    mix(std::to_string(ids[index]).c_str());
    mix(SDL_GetJoystickNameForID(ids[index]));
    mix(SDL_IsGamepad(ids[index]) ? "g" : "j");
  }
  if (ids != nullptr) SDL_free(ids);
  return hash ^ static_cast<std::uint64_t>(count);
}

#ifdef _WIN32
PadState ControllerInput::poll_xinput(bool focused) {
  PadState pad;
  if (!focused) {
    stop();
    return pad;
  }
  XINPUT_STATE state{};
  if (slot_ == XUSER_MAX_COUNT && GetTickCount64() >= next_scan_) {
    next_scan_ = GetTickCount64() + 2000;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
      const DWORD result = XInputGetState(slot, &state);
      if (result == ERROR_SUCCESS) {
        slot_ = slot;
        std::fprintf(stderr, "[input] XInput controller slot %lu connected\n",
                     slot);
        break;
      }
      if (result != ERROR_DEVICE_NOT_CONNECTED)
        throw std::runtime_error("XInputGetState failed: " +
                                 std::to_string(result));
    }
  }
  if (slot_ != XUSER_MAX_COUNT) {
    const DWORD result = XInputGetState(slot_, &state);
    if (result == ERROR_SUCCESS)
      pad = map_gamepad(state.Gamepad, map_);
    else if (result == ERROR_DEVICE_NOT_CONNECTED) {
      std::fprintf(stderr, "[input] XInput controller disconnected\n");
      slot_ = XUSER_MAX_COUNT;
      motors_ = 0;
      next_scan_ = 0;
    } else
      throw std::runtime_error("XInputGetState failed: " +
                               std::to_string(result));
  }
  return pad;
}
#endif

// SDL_GetGamepadAxis has already normalized every mapped gamepad trigger to 0..32767,
// including DualSense. Device names and raw joystick/HID ranges must not rescale it again.
gt4::pad::GamepadReading read_sdl_gamepad(SDL_Gamepad *gamepad) {
  gt4::pad::GamepadReading reading;
  // SDL face buttons are positions: SOUTH is Cross on a PlayStation pad and A on an Xbox pad.
  const struct {
    SDL_GamepadButton button;
    std::uint16_t mask;
  } buttons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, gt4::controls::xinput::a},
      {SDL_GAMEPAD_BUTTON_EAST, gt4::controls::xinput::b},
      {SDL_GAMEPAD_BUTTON_WEST, gt4::controls::xinput::x},
      {SDL_GAMEPAD_BUTTON_NORTH, gt4::controls::xinput::y},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, gt4::controls::xinput::left_shoulder},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, gt4::controls::xinput::right_shoulder},
      {SDL_GAMEPAD_BUTTON_BACK, gt4::controls::xinput::back},
      {SDL_GAMEPAD_BUTTON_START, gt4::controls::xinput::start},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, gt4::controls::xinput::left_thumb},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, gt4::controls::xinput::right_thumb},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, gt4::controls::xinput::dpad_up},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, gt4::controls::xinput::dpad_down},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, gt4::controls::xinput::dpad_left},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, gt4::controls::xinput::dpad_right},
  };
  for (const auto &entry : buttons)
    if (SDL_GetGamepadButton(gamepad, entry.button))
      reading.buttons |= entry.mask;
  reading.left_trigger = gt4::pad::trigger_byte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  reading.right_trigger = gt4::pad::trigger_byte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  reading.thumb_lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  reading.thumb_ly = gt4::pad::negate_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
  reading.thumb_rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
  reading.thumb_ry = gt4::pad::negate_axis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
  return reading;
}

PadState ControllerInput::poll_sdl(bool focused) {
  PadState pad;
  // Refresh the gamepad states once per poll (SDL does not pump them on its own without events).
  SDL_UpdateGamepads();
  if (sdl_pad_ && !SDL_GamepadConnected(sdl_pad_)) {
    std::fprintf(stderr, "[input] SDL gamepad disconnected\n");
    close_sdl_pad();
    motors_ = 0;
    sdl_next_scan_ = 0;
  }
  // Scan when no pad is open, and (TASK-148) while a pad stands in for the chosen one.
  if ((!sdl_pad_ || (has_pad_choice_ && !reading_choice_)) && gt4::platform::monotonic_ms() >= sdl_next_scan_) {
    sdl_next_scan_ = gt4::platform::monotonic_ms() + 2000;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    std::vector<gt4::pad::PadCandidate> candidates;
    for (int index = 0; ids && index < count; ++index) {
      if (is_excluded(ids[index]))
        continue;
      gt4::pad::PadCandidate candidate;
      candidate.instance = ids[index];
      char guid[33] = {};
      SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[index]), guid, sizeof guid);
      candidate.guid = guid;
      // The serial is read only to tell one of two pads of the chosen model apart (it needs the pad open).
      if (has_pad_choice_ && !pad_choice_.serial.empty()) {
        gt4::pad::PadChoice model;
        if (gt4::pad::parse_pad_choice(guid, model) && model.guid == pad_choice_.guid) {
          if (SDL_Gamepad *probe = SDL_OpenGamepad(ids[index])) {
            const char *serial = SDL_GetGamepadSerial(probe);
            candidate.serial = serial ? serial : "";
            SDL_CloseGamepad(probe);
          }
        }
      }
      candidates.push_back(candidate);
    }
    std::uint32_t chosen = 0;
    if (gt4::pad::choose_pad(candidates, has_pad_choice_ ? &pad_choice_ : nullptr, chosen) &&
        (!sdl_pad_ || SDL_GetGamepadID(sdl_pad_) != chosen)) {
      close_sdl_pad();
      motors_ = 0;
      sdl_pad_ = SDL_OpenGamepad(chosen);
      if (sdl_pad_) {
        const char *name = SDL_GetGamepadName(sdl_pad_);
        gt4::pad::PadChoice model;
        char guid[33] = {};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(chosen), guid, sizeof guid);
        reading_choice_ = has_pad_choice_ && gt4::pad::parse_pad_choice(guid, model) && model.guid == pad_choice_.guid;
        if (reading_choice_)
          std::fprintf(stderr, "[input] SDL gamepad \"%s\" is the controller chosen in the launcher (%d connected)\n",
                       name ? name : "unnamed", count);
        else if (has_pad_choice_)
          std::fprintf(stderr, "[input] the chosen controller is not connected: SDL gamepad \"%s\" is controller 1 of %d\n",
                       name ? name : "unnamed", count);
        else
          std::fprintf(stderr, "[input] SDL gamepad \"%s\" is controller 1 of %d\n", name ? name : "unnamed", count);
      }
    }
    SDL_free(ids);
  }
  if (!sdl_pad_ || !focused) {
    stop();
    return pad;
  }
  return gt4::pad::map_reading(read_sdl_gamepad(sdl_pad_), map_);
}

// TASK-180 test3: the `[input]` block (see the header). It answers the player's "the pad does nothing in the game" with
// data: which devices SDL enumerated, whether each got a gamepad mapping (an unmapped joystick is never listed as a
// gamepad, so the game cannot open it as one), and which instance the game reads. The window's input focus is part of
// it because the pad is read as neutral while the window does not have it.
std::string ControllerInput::reading_text() const {
  if (backend_ == gt4::pad::Backend::xinput)
    return "";  // the XInput slot is reported by log_devices
  if (sdl_pad_ == nullptr)
    return "";
  const char *name = SDL_GetGamepadName(sdl_pad_);
  return std::string("\"") + (name != nullptr ? name : "unnamed") + "\" (instance " +
         std::to_string(SDL_GetGamepadID(sdl_pad_)) + (reading_choice_ ? ", the launcher's choice" : "") + ")";
}

void ControllerInput::log_devices(const char *reason, bool window_focused) const {
  std::fprintf(stderr, "[input] === %s: backend %s, window focus %s ===\n", reason != nullptr ? reason : "input",
               gt4::pad::backend_name(backend_), window_focused ? "yes" : "NO");
  if (backend_ == gt4::pad::Backend::xinput) {
#ifdef _WIN32
    if (slot_ == XUSER_MAX_COUNT)
      std::fprintf(stderr, "[input] XInput: no controller connected; the game reads the keyboard only\n");
    else
      std::fprintf(stderr, "[input] reading XInput slot %lu\n", static_cast<unsigned long>(slot_));
#endif
    return;
  }
  int count = 0;
  SDL_JoystickID *ids = SDL_GetJoysticks(&count);
  std::fprintf(stderr, "[input] devices: %d connected%s\n", count, has_pad_choice_ ? ", the launcher's controller chosen" : "");
  for (int index = 0; ids != nullptr && index < count; ++index) {
    const SDL_JoystickID id = ids[index];
    const char *name = SDL_GetJoystickNameForID(id);
    const bool gamepad = SDL_IsGamepad(id);
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    int axes = 0, buttons = 0, hats = 0;
    // Counts need the device open; the game opens it anyway unless the wheel owns it, and opening twice is allowed.
    const bool excluded = is_excluded(id);
    if (SDL_Joystick *probe = excluded ? nullptr : SDL_OpenJoystick(id)) {
      axes = SDL_GetNumJoystickAxes(probe);
      buttons = SDL_GetNumJoystickButtons(probe);
      SDL_CloseJoystick(probe);
    }
    const char *mapping = gamepad ? SDL_GetGamepadMappingForID(id) : nullptr;
    const char *type = gamepad ? "gamepad" : "joystick, no gamepad mapping";
    std::fprintf(stderr,
                 "[input]   %d: \"%s\" vid:pid %04x:%04x guid %s, %s, %d axes, %d buttons, %s%s%s\n", index + 1,
                 name != nullptr ? name : "unnamed", static_cast<unsigned>(SDL_GetJoystickVendorForID(id)),
                 static_cast<unsigned>(SDL_GetJoystickProductForID(id)), guid, type, axes, buttons,
                 excluded ? "owned by the wheel" : (sdl_pad_ != nullptr && SDL_GetGamepadID(sdl_pad_) == id) ? "READ" : "-",
                 mapping != nullptr ? ", mapping: " : "", mapping != nullptr ? mapping : "");
    if (mapping != nullptr) SDL_free(const_cast<char *>(mapping));
  }
  if (ids != nullptr) SDL_free(ids);
  const std::string reading = reading_text();
  bool any_gamepad = false;
  for (int index = 0; ids != nullptr && index < count; ++index) any_gamepad |= SDL_IsGamepad(ids[index]);
  if (!reading.empty())
    std::fprintf(stderr, "[input] reading %s\n", reading.c_str());
  else if (any_gamepad)
    std::fprintf(stderr, "[input] reading nothing yet: the gamepad above is not open at this moment (the scan opens it at "
                         "the next VBlank)\n");
  else if (count > 0)
    std::fprintf(stderr, "[input] reading nothing: none of the devices is a gamepad (a joystick without a gamepad mapping "
                         "needs SDL_GAMECONTROLLERCONFIG_FILE); the game reads the keyboard only\n");
  else
    std::fprintf(stderr, "[input] reading nothing: no device; the game reads the keyboard only\n");
}

void ControllerInput::rumble(std::uint16_t value) {
#ifdef _WIN32
  if (backend_ == gt4::pad::Backend::sdl)
    rumble_sdl(value);
  else
    rumble_xinput(value);
#else
  rumble_sdl(value);
#endif
}

#ifdef _WIN32
void ControllerInput::rumble_xinput(std::uint16_t value) {
  if (slot_ == XUSER_MAX_COUNT || motors_ == value)
    return;
  auto vibration = map_rumble(value);
  const DWORD result = XInputSetState(slot_, &vibration);
  if (result == ERROR_DEVICE_NOT_CONNECTED) {
    slot_ = XUSER_MAX_COUNT;
    motors_ = 0;
    next_scan_ = 0;
    return;
  }
  if (result != ERROR_SUCCESS)
    throw std::runtime_error("XInputSetState failed: " +
                             std::to_string(result));
  motors_ = value;
}
#endif

void ControllerInput::rumble_sdl(std::uint16_t value) {
  if (!sdl_pad_)
    return;
  const std::uint64_t now = gt4::platform::monotonic_ms();
  // SDL rumble lasts for its duration: resend before it runs out while the value holds.
  if (motors_ == value && now - rumble_sent_at_ < 250)
    return;
  const auto motors = gt4::pad::rumble_motors(value);
  // A pad without rumble support refuses the call; that is not an error for the game.
  SDL_RumbleGamepad(sdl_pad_, motors.low_frequency, motors.high_frequency, 500);
  motors_ = value;
  rumble_sent_at_ = now;
}

void ControllerInput::stop() {
  if (backend_ == gt4::pad::Backend::sdl) {
    if (sdl_pad_ && motors_)
      SDL_RumbleGamepad(sdl_pad_, 0, 0, 0);
    motors_ = 0;
    return;
  }
#ifdef _WIN32
  stop_xinput();
#endif
}

#ifdef _WIN32
void ControllerInput::stop_xinput() {
  if (slot_ != XUSER_MAX_COUNT && motors_) {
    XINPUT_VIBRATION vibration{};
    const DWORD result = XInputSetState(slot_, &vibration);
    if (result != ERROR_SUCCESS && result != ERROR_DEVICE_NOT_CONNECTED) {
      std::fprintf(stderr, "[input] stopping rumble failed: %lu\n", result);
      return;
    }
  }
  motors_ = 0;
}
#endif

namespace {
// Blocking audio path: four 512-frame buffers, as before the paced mode.
constexpr std::size_t blocking_frames_per_buffer = 512;
constexpr std::size_t blocking_buffer_count = 4;
// Paced path: sixteen 256-frame buffers (5.3 ms each) under a 60 ms target.
constexpr std::size_t paced_frames_per_buffer = 256;
constexpr std::size_t paced_buffer_count = 16;
// TASK-156: under Wine, twenty-four 512-frame buffers (10.7 ms each) under a 100 ms target.
constexpr std::size_t wine_frames_per_buffer = 512;
constexpr std::size_t wine_buffer_count = 24;
constexpr std::size_t wine_target_frames = 4800;
} // namespace

PacedAudioProfile choose_paced_audio_profile(bool wine, const char *latency_ms) {
  PacedAudioProfile profile = wine ? PacedAudioProfile{wine_frames_per_buffer, wine_buffer_count, wine_target_frames}
                                   : PacedAudioProfile{paced_frames_per_buffer, paced_buffer_count,
                                                       gt4::pacing::target_latency_frames};
  if (latency_ms != nullptr && *latency_ms != '\0') {
    char *end = nullptr;
    const long ms = std::strtol(latency_ms, &end, 10);
    if (end != latency_ms && *end == '\0' && ms >= 20 && ms <= 500)
      profile.target_frames = static_cast<std::size_t>(ms) * 48u;  // 48 frames per millisecond at 48 kHz
    else
      std::fprintf(stderr, "[audio] GT4_AUDIO_LATENCY_MS=%s is not 20..500: the default target is kept\n", latency_ms);
  }
  return profile;
}

#ifdef _WIN32
bool running_under_wine() {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

AudioOutput::AudioOutput(std::function<bool()> pump_messages, gt4::pacing::Mode mode, AudioDevice requested)
    : mode_(mode), pump_messages_(std::move(pump_messages)) {
  const bool paced = mode_ == gt4::pacing::Mode::paced;
  const bool wine = paced && running_under_wine();
  frames_per_buffer_ = blocking_frames_per_buffer;
  buffer_count_ = blocking_buffer_count;
  if (paced) {
    const PacedAudioProfile profile = choose_paced_audio_profile(wine, std::getenv("GT4_AUDIO_LATENCY_MS"));
    frames_per_buffer_ = profile.frames_per_buffer;
    buffer_count_ = profile.buffer_count;
    target_latency_frames_ = profile.target_frames;
    controller_ = gt4::pacing::RateController{static_cast<double>(target_latency_frames_),
                                              1.0 / static_cast<double>(gt4::pacing::guest_vblank_rate)};
  }
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event_)
    throw std::runtime_error("Cannot create audio completion event");
  try {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 192000;
    // TASK-129: no device (a PC without speakers or with every output disabled answers MMSYSERR_BADDEVICEID), or any
    // failure to open it, leaves the game without sound but running. Failures after a successful open still stop it.
    const UINT devices = waveOutGetNumDevs();
    MMRESULT opened = MMSYSERR_BADDEVICEID;
    if (requested == AudioDevice::system && devices != 0)
      opened = waveOutOpen(&device_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT);
    if (opened != MMSYSERR_NOERROR) {
      char description[MAXERRORLENGTH]{};
      if (requested == AudioDevice::system)
        waveOutGetErrorTextA(opened, description, MAXERRORLENGTH);
      device_ = nullptr;
      silent_ = true;
      std::fprintf(stderr, "[audio] no sound: %s (%u output devices, result %u); the game runs without audio\n",
                   requested == AudioDevice::none ? "GT4_AUDIO=none" : description, devices,
                   static_cast<unsigned>(opened));
      return;
    }
    for (std::size_t index = 0; index < buffer_count_; ++index) {
      auto &buffer = buffers_[index];
      buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
      buffer.header.dwBufferLength =
          static_cast<DWORD>(frames_per_buffer_ * 2 * sizeof(std::int16_t));
      check_audio(
          waveOutPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)),
          "waveOutPrepareHeader");
      buffer.prepared = true;
    }
    if (paced)
      std::fprintf(stderr,
                   "[audio] Windows PCM stereo16 48000 Hz, paced: %zu x %zu "
                   "frames, %zu ms target%s, non-blocking, rate control +-0.5 percent\n",
                   buffer_count_, frames_per_buffer_, target_latency_frames_ / 48u, wine ? " (Wine)" : "");
    else
      std::fprintf(stderr,
                   "[audio] Windows PCM stereo16 48000 Hz, 4 x 512 frames\n");
  } catch (...) {
    try {
      close(false);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "[audio] fatal construction cleanup failure: %s\n",
                   error.what());
      std::terminate();
    }
    throw;
  }
}

AudioOutput::~AudioOutput() {
  try {
    close(false);
  } catch (const std::exception &error) {
    // Never release storage still owned by a failed audio driver.
    std::fprintf(stderr, "[audio] fatal cleanup failure: %s\n", error.what());
    std::terminate();
  }
}

std::size_t AudioOutput::reap() {
  std::size_t queued = 0;
  for (std::size_t index = 0; index < buffer_count_; ++index) {
    auto &buffer = buffers_[index];
    if (buffer.queued && (buffer.header.dwFlags & WHDR_DONE)) {
      completed_ += buffer.header.dwBufferLength / 4;
#if defined(GT4_PERF_PROBE) && !defined(GT4_PERF_FRAME_ONLY)
      gt4::perf::Probe::shared().audio(0, buffer.header.dwBufferLength / 4);
#endif
      buffer.queued = false;
    }
    queued += buffer.queued;
  }
  return queued;
}

std::size_t AudioOutput::queued_device_buffers() const {
  std::size_t queued = 0;
  for (std::size_t index = 0; index < buffer_count_; ++index)
    queued += buffers_[index].queued;
  return queued;
}

bool AudioOutput::wait_for(Buffer *buffer) {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    // Reset before rescanning headers: coalesced notifications cannot lose
    // completion.
    if (!ResetEvent(event_))
      throw std::runtime_error("Cannot reset audio completion event");
    const auto queued = reap();
    if (buffer ? !buffer->queued : queued == 0)
      return true;
    if (pump_messages_ && !pump_messages_())
      return false;
    if (GetTickCount64() >= deadline)
      throw std::runtime_error("Windows audio device stalled for five seconds");
    ++waits_;
    DWORD result;
    {
      GT4_PERF_SCOPE(gt4::perf::Category::audio_wait);
      result = MsgWaitForMultipleObjects(1, &event_, FALSE, 50, QS_ALLINPUT);
    }
    if (result == WAIT_FAILED)
      throw std::runtime_error("Waiting for audio completion failed");
  }
}

bool AudioOutput::submit() {
  if (!pending_frames_)
    return true;
  auto &buffer = buffers_[next_buffer_];
  if (!wait_for(&buffer))
    return false;
  const auto queued = reap();
  if (!queued && submitted_) {
    ++empty_queue_;
#if defined(GT4_PERF_PROBE) && !defined(GT4_PERF_FRAME_ONLY)
    gt4::perf::Probe::shared().audio(0, 0, 1);
#endif
  }
  std::copy_n(pending_.data(), pending_frames_ * 2, buffer.samples.data());
  buffer.header.dwBufferLength = static_cast<DWORD>(pending_frames_ * 4);
  check_audio(waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)),
              "waveOutWrite");
  buffer.queued = true;
  submitted_ += pending_frames_;
  peak_ = std::max(peak_, queued + 1);
  pending_frames_ = 0;
  next_buffer_ = (next_buffer_ + 1) % buffer_count_;
  return true;
}

void AudioOutput::submit_paced_block() {
  auto &buffer = buffers_[next_buffer_];
  // Callers check that at least one full block is in the ring.
  ring_.read(buffer.samples.data(), frames_per_buffer_);
  buffer.header.dwBufferLength = static_cast<DWORD>(frames_per_buffer_ * 4);
  check_audio(waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)),
              "waveOutWrite");
  buffer.queued = true;
  submitted_ += frames_per_buffer_;
  peak_ = std::max(peak_, queued_device_buffers());
  next_buffer_ = (next_buffer_ + 1) % buffer_count_;
}

bool AudioOutput::push_paced(const std::int16_t *samples, std::size_t frames) {
  converted_.clear();
  const std::size_t produced = resampler_.process(samples, frames, converted_);
  const std::size_t accepted = ring_.write(converted_.data(), produced);
  overflow_frames_ += produced - accepted;
  return true;
}

bool AudioOutput::push(const std::int16_t *samples, std::size_t frames) {
  GT4_PERF_SCOPE(gt4::perf::Category::audio_push);
  if (closed_ || (!device_ && !silent_) || (frames && !samples))
    throw std::runtime_error("Invalid audio sink input/lifetime");
  received_ += frames;
  if (silent_)
    return true;
#if defined(GT4_PERF_PROBE) && !defined(GT4_PERF_FRAME_ONLY)
  gt4::perf::Probe::shared().audio(frames, 0);
#endif
  if (mode_ == gt4::pacing::Mode::paced)
    return push_paced(samples, frames);
  while (frames) {
    const auto count = std::min(frames, frames_per_buffer_ - pending_frames_);
    std::copy_n(samples, count * 2, pending_.data() + pending_frames_ * 2);
    samples += count * 2;
    frames -= count;
    pending_frames_ += count;
    if (pending_frames_ == frames_per_buffer_ && !submit())
      return false;
  }
  return true;
}

void AudioOutput::pump() {
  if (mode_ != gt4::pacing::Mode::paced || !device_)
    return;
  reap();
  if (!started_ && ring_.size() >= target_latency_frames_)
    started_ = true;
  if (!started_)
    return;
  // Nothing queued on the device after audio was submitted: the device starved.
  if (!queued_device_buffers() && submitted_)
    ++underruns_;
  // The controller sees the whole queue: device plus ring.
  resampler_.set_ratio(controller_.update(static_cast<double>(queued_frames())));
  // Submit only while the device stays within the target: the excess waits in
  // the ring, where the controller can see it.
  while (ring_.size() >= frames_per_buffer_ &&
         (queued_device_buffers() + 1) * frames_per_buffer_ <= target_latency_frames_ &&
         !buffers_[next_buffer_].queued)
    submit_paced_block();
}

std::uint64_t AudioOutput::queued_frames() {
  reap();
  const std::uint64_t device = queued_device_buffers() * frames_per_buffer_;
  return device + (mode_ == gt4::pacing::Mode::paced ? ring_.size() : pending_frames_);
}

void AudioOutput::close(bool drain) {
  if (device_) {
    if (drain && submit())
      wait_for(nullptr);
    reap();
    check_audio(waveOutReset(device_), "waveOutReset");
    for (std::size_t index = 0; index < buffer_count_; ++index) {
      auto &buffer = buffers_[index];
      if (buffer.queued) {
        canceled_ += buffer.header.dwBufferLength / 4;
        buffer.queued = false;
      }
      if (buffer.prepared) {
        check_audio(
            waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR)),
            "waveOutUnprepareHeader");
        buffer.prepared = false;
      }
    }
    check_audio(waveOutClose(device_), "waveOutClose");
    device_ = nullptr;
    std::fprintf(
        stderr,
        "[audio] received=%llu submitted=%llu completed=%llu canceled=%llu "
        "pending=%zu peak=%zu waits=%llu empty_queue=%llu\n",
        static_cast<unsigned long long>(received_),
        static_cast<unsigned long long>(submitted_),
        static_cast<unsigned long long>(completed_),
        static_cast<unsigned long long>(canceled_), pending_frames_, peak_,
        static_cast<unsigned long long>(waits_),
        static_cast<unsigned long long>(empty_queue_));
    if (mode_ == gt4::pacing::Mode::paced)
      std::fprintf(stderr,
                   "[audio] paced: underruns=%llu ring_overflow_frames=%llu "
                   "ring_left=%zu ratio=%.6f\n",
                   static_cast<unsigned long long>(underruns_),
                   static_cast<unsigned long long>(overflow_frames_),
                   ring_.size(), resampler_.ratio());
  }
  if (silent_ && !closed_)
    std::fprintf(stderr, "[audio] silent: received=%llu frames dropped\n",
                 static_cast<unsigned long long>(received_));
  closed_ = true;
  if (event_) {
    CloseHandle(event_);
    event_ = nullptr;
  }
}
#endif  // _WIN32: the WinMM AudioOutput; audio_sdl.cpp is the SDL3 form
