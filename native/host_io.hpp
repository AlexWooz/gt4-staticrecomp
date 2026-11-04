#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <mmsystem.h>
#include <Xinput.h>
// clang-format on

#include "controls.hpp"
#include "pacing.hpp"
#include "pad_mapping.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// TASK-075: the PS2 pad state lives in pad_mapping.hpp (shared with the SDL3 path).
using PadState = gt4::pad::PadState;

// SDL3 gamepad handle (SDL_gamepad.h); the header is kept out of this file.
struct SDL_Gamepad;

// One standardized SDL3 gamepad reading, shared by live polling and the virtual-device fixture.
gt4::pad::GamepadReading read_sdl_gamepad(SDL_Gamepad *gamepad);

// Maps one XInput state through the binding table. The default-argument overload
// uses controls::default_controls(), which equals the slice 157 mapping.
PadState map_gamepad(const XINPUT_GAMEPAD &gamepad, const gt4::controls::ControlMap &map);
PadState map_gamepad(const XINPUT_GAMEPAD &gamepad);
void merge_keyboard(PadState &pad, std::uint16_t buttons);
XINPUT_VIBRATION map_rumble(std::uint16_t rumble);

// Player 1's pad. GT4_INPUT=sdl (default): SDL3 gamepad subsystem only, no SDL window; the first connected pad is
// player 1, and hot-plug is polled. GT4_INPUT=xinput: the previous XInput path.
class ControllerInput {
public:
  explicit ControllerInput(gt4::controls::ControlMap map = gt4::controls::default_controls(),
                           gt4::pad::Backend backend = gt4::pad::Backend::sdl);
  ~ControllerInput();
  ControllerInput(const ControllerInput &) = delete;
  ControllerInput &operator=(const ControllerInput &) = delete;
  PadState poll(bool focused, std::uint16_t keyboard_buttons);
  void rumble(std::uint16_t value);
  void stop();
  gt4::pad::Backend backend() const { return backend_; }

private:
  PadState poll_xinput(bool focused);
  PadState poll_sdl(bool focused);
  void rumble_xinput(std::uint16_t value);
  void rumble_sdl(std::uint16_t value);
  void close_sdl_pad();

  gt4::controls::ControlMap map_;
  gt4::pad::Backend backend_;
  DWORD slot_ = XUSER_MAX_COUNT;
  ULONGLONG next_scan_ = 0;
  SDL_Gamepad *sdl_pad_ = nullptr;
  ULONGLONG sdl_next_scan_ = 0;
  // TASK-148: the controller chosen in the launcher (GT4_PAD), and whether the open pad is it. While another pad stands in
  // for it, the scan keeps looking for it every two seconds.
  gt4::pad::PadChoice pad_choice_;
  bool has_pad_choice_ = false;
  bool reading_choice_ = false;
  ULONGLONG rumble_sent_at_ = 0;
  bool sdl_initialized_ = false;
  std::uint16_t motors_ = 0;
};

// The output the game asks for: the Windows default device, or none (GT4_AUDIO=none, and the fixture).
enum class AudioDevice { system, none };

// TASK-156 (issue #129; re-done from Luan Antony's PR #141): the paced audio path's buffers and target. On Windows sixteen
// 256-frame buffers under a 60 ms target, as before. Under Wine (Proton, SteamOS) waveOut on PipeWire pulls about 1024
// frames at a time, so that queue ran dry between pulls (crackling engine notes): twenty-four 512-frame buffers under
// 100 ms. GT4_AUDIO_LATENCY_MS=20..500 sets only the target, keeping the buffers of the system detected.
struct PacedAudioProfile {
  std::size_t frames_per_buffer;
  std::size_t buffer_count;
  std::size_t target_frames;
};
PacedAudioProfile choose_paced_audio_profile(bool wine, const char *latency_ms);
// True when the game runs under Wine (ntdll exports wine_get_version).
bool running_under_wine();

class AudioOutput {
public:
  // `mode` selects the blocking WinMM path (audio) or the non-blocking paced
  // ring with rate control (paced); see pacing.hpp.
  // TASK-129 (issues #93, #115): when Windows has no output device, or opening it fails, the output is silent instead of
  // stopping the game: push() accepts and drops the samples, and silent() is true.
  explicit AudioOutput(std::function<bool()> pump_messages = {},
                       gt4::pacing::Mode mode = gt4::pacing::Mode::audio,
                       AudioDevice requested = AudioDevice::system);
  ~AudioOutput();
  AudioOutput(const AudioOutput &) = delete;
  AudioOutput &operator=(const AudioOutput &) = delete;
  // Audio mode waits for a free device buffer. Paced mode never waits: the
  // frames are resampled into the ring.
  bool push(const std::int16_t *samples, std::size_t frames);
  // Paced mode: reaps finished buffers, updates the rate ratio and submits full
  // blocks while the device queue is below the latency target. Never waits.
  // Does nothing in audio mode.
  void pump();
  // Frames queued on the device plus the ring (paced) or pending (audio).
  std::uint64_t queued_frames();
  gt4::pacing::Mode mode() const { return mode_; }
  bool silent() const { return silent_; }
  double resampler_ratio() const { return resampler_.ratio(); }
  std::uint64_t underruns() const { return underruns_; }
  std::size_t target_latency_frames() const { return target_latency_frames_; }
  std::uint64_t overflow_frames() const { return overflow_frames_; }
  std::uint64_t received_frames() const { return received_; }
  std::uint64_t completed_frames() const { return completed_; }
  std::uint64_t canceled_frames() const { return canceled_; }
  void close(bool drain);

private:
  static constexpr std::size_t max_frames_per_buffer = 512;
  static constexpr std::size_t max_buffers = 24;  // TASK-156: the Wine profile's buffer count
  struct Buffer {
    std::array<std::int16_t, max_frames_per_buffer * 2> samples{};
    WAVEHDR header{};
    bool prepared = false;
    bool queued = false;
  };
  std::array<Buffer, max_buffers> buffers_{};
  std::array<std::int16_t, max_frames_per_buffer * 2> pending_{};
  std::size_t pending_frames_ = 0;
  std::size_t frames_per_buffer_ = 512;
  std::size_t buffer_count_ = 4;
  std::size_t target_latency_frames_ = gt4::pacing::target_latency_frames;  // TASK-156: the paced profile's target
  std::size_t next_buffer_ = 0;
  gt4::pacing::Mode mode_ = gt4::pacing::Mode::audio;
  bool silent_ = false;
  bool closed_ = false;
  bool started_ = false;
  gt4::pacing::FrameRing ring_{gt4::pacing::paced_ring_frames};
  gt4::pacing::LinearResampler resampler_;
  gt4::pacing::RateController controller_{
      static_cast<double>(gt4::pacing::target_latency_frames),
      1.0 / static_cast<double>(gt4::pacing::guest_vblank_rate)};
  std::vector<std::int16_t> converted_;
  HWAVEOUT device_ = nullptr;
  HANDLE event_ = nullptr;
  std::function<bool()> pump_messages_;
  std::uint64_t received_ = 0, submitted_ = 0, completed_ = 0, canceled_ = 0;
  std::uint64_t empty_queue_ = 0, waits_ = 0;
  std::uint64_t underruns_ = 0, overflow_frames_ = 0;
  std::size_t peak_ = 0;
  std::size_t reap();
  std::size_t queued_device_buffers() const;
  bool wait_for(Buffer *buffer);
  bool submit();
  bool push_paced(const std::int16_t *samples, std::size_t frames);
  void submit_paced_block();
};
