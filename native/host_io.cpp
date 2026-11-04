#include "host_io.hpp"
#include "perf_probe.hpp"

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

namespace {
void check_audio(MMRESULT result, const char *operation) {
  if (result == MMSYSERR_NOERROR)
    return;
  char description[MAXERRORLENGTH]{};
  waveOutGetErrorTextA(result, description, MAXERRORLENGTH);
  throw std::runtime_error(std::string(operation) + ": " + description + " (" +
                           std::to_string(result) + ")");
}

void press(PadState &pad, std::uint8_t bit, std::uint8_t pressure) {
  pad.buttons |= static_cast<std::uint16_t>(1u << bit);
  pad.pressures[bit] = std::max(pad.pressures[bit], pressure);
}
} // namespace

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

void merge_keyboard(PadState &pad, std::uint16_t buttons) {
  pad.buttons |= buttons;
  for (int bit = 0; bit < 16; ++bit)
    if (buttons & (1u << bit))
      pad.pressures[bit] = 255;
}

XINPUT_VIBRATION map_rumble(std::uint16_t rumble) {
  const auto motors = gt4::pad::rumble_motors(rumble);
  return {static_cast<WORD>(motors.low_frequency), static_cast<WORD>(motors.high_frequency)};
}

ControllerInput::ControllerInput(ControlMap map, gt4::pad::Backend backend)
    : map_(std::move(map)), backend_(backend) {
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

PadState ControllerInput::poll(bool focused, std::uint16_t keyboard_buttons) {
  PadState pad = backend_ == gt4::pad::Backend::sdl ? poll_sdl(focused) : poll_xinput(focused);
  // Without focus the pad is neutral: the keyboard counts only in the game window.
  if (!focused)
    return pad;
  merge_keyboard(pad, keyboard_buttons);
  return pad;
}

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
  if ((!sdl_pad_ || (has_pad_choice_ && !reading_choice_)) && GetTickCount64() >= sdl_next_scan_) {
    sdl_next_scan_ = GetTickCount64() + 2000;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    std::vector<gt4::pad::PadCandidate> candidates;
    for (int index = 0; ids && index < count; ++index) {
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

void ControllerInput::rumble(std::uint16_t value) {
  if (backend_ == gt4::pad::Backend::sdl)
    rumble_sdl(value);
  else
    rumble_xinput(value);
}

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

void ControllerInput::rumble_sdl(std::uint16_t value) {
  if (!sdl_pad_)
    return;
  const ULONGLONG now = GetTickCount64();
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

namespace {
// Blocking audio path: four 512-frame buffers, as before the paced mode.
constexpr std::size_t blocking_frames_per_buffer = 512;
constexpr std::size_t blocking_buffer_count = 4;
// Paced path: sixteen 256-frame buffers (5.3 ms each) under a 60 ms target.
constexpr std::size_t paced_frames_per_buffer = 256;
constexpr std::size_t paced_buffer_count = 16;
} // namespace

AudioOutput::AudioOutput(std::function<bool()> pump_messages, gt4::pacing::Mode mode, AudioDevice requested)
    : mode_(mode), pump_messages_(std::move(pump_messages)) {
  const bool paced = mode_ == gt4::pacing::Mode::paced;
  frames_per_buffer_ = paced ? paced_frames_per_buffer : blocking_frames_per_buffer;
  buffer_count_ = paced ? paced_buffer_count : blocking_buffer_count;
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
                   "frames, 60 ms target, non-blocking, rate control +-0.5 percent\n",
                   buffer_count_, frames_per_buffer_);
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
  if (!started_ && ring_.size() >= gt4::pacing::target_latency_frames)
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
         (queued_device_buffers() + 1) * frames_per_buffer_ <=
             gt4::pacing::target_latency_frames &&
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
