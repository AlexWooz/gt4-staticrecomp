// SPDX-License-Identifier: MIT
// TASK-180: AudioOutput (host_io.hpp) on an SDL3 audio stream, for the platforms without WinMM (Android first, Linux
// next). The two modes of the Windows output are kept:
//   - paced (the default): the game's samples are resampled into the ring; pump() moves whole blocks from the ring to
//     the device while the device queue stays within the latency target, and the rate controller sees device + ring;
//   - audio (GT4_PACING=audio): push() waits while the device holds more than four blocks, so audio paces the game.
// SDL converts 48 kHz stereo S16 to the device format itself (AAudio or OpenSL ES on Android). With no device, or when it
// cannot be opened, the output is silent and the game runs on (TASK-129), exactly as on Windows.
#include "host_io.hpp"
#include "perf_probe.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
constexpr int kChannels = 2;
constexpr std::size_t kBytesPerFrame = kChannels * sizeof(std::int16_t);
// The blocking path waits while more than this many blocks are queued (the Windows path has four 512-frame buffers).
constexpr std::size_t kBlockingBlocks = 4;
constexpr std::size_t kBlockingFramesPerBuffer = 512;
}  // namespace

bool running_under_wine() { return false; }

AudioOutput::AudioOutput(std::function<bool()> pump_messages, gt4::pacing::Mode mode, AudioDevice requested)
    : mode_(mode), pump_messages_(std::move(pump_messages)) {
  const bool paced = mode_ == gt4::pacing::Mode::paced;
  frames_per_buffer_ = kBlockingFramesPerBuffer;
  if (paced) {
    const PacedAudioProfile profile = choose_paced_audio_profile(false, std::getenv("GT4_AUDIO_LATENCY_MS"));
    frames_per_buffer_ = profile.frames_per_buffer;
    target_latency_frames_ = profile.target_frames;
    controller_ = gt4::pacing::RateController{static_cast<double>(target_latency_frames_),
                                              1.0 / static_cast<double>(gt4::pacing::guest_vblank_rate)};
  }
  block_.resize(frames_per_buffer_ * kChannels);
  if (requested == AudioDevice::none) {
    silent_ = true;
    std::fprintf(stderr, "[audio] no sound: GT4_AUDIO=none; the game runs without audio\n");
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    silent_ = true;
    std::fprintf(stderr, "[audio] no sound: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s; the game runs without audio\n",
                 SDL_GetError());
    return;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_S16, kChannels, static_cast<int>(gt4::pacing::sample_rate_hz)};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (stream_ == nullptr) {
    silent_ = true;
    std::fprintf(stderr, "[audio] no sound: %s; the game runs without audio\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  if (paced)
    std::fprintf(stderr,
                 "[audio] SDL3 %s, PCM stereo16 48000 Hz, paced: %zu-frame blocks, %zu ms target, non-blocking, rate "
                 "control +-0.5 percent\n",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "audio", frames_per_buffer_,
                 target_latency_frames_ / 48u);
  else
    std::fprintf(stderr, "[audio] SDL3 %s, PCM stereo16 48000 Hz, blocking: %zu x %zu frames\n",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "audio", kBlockingBlocks,
                 frames_per_buffer_);
}

AudioOutput::~AudioOutput() {
  try {
    close(false);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[audio] cleanup failure: %s\n", error.what());
  }
}

std::size_t AudioOutput::device_queued_frames() const {
  if (stream_ == nullptr) return 0;
  const int bytes = SDL_GetAudioStreamQueued(stream_);
  return bytes > 0 ? static_cast<std::size_t>(bytes) / kBytesPerFrame : 0;
}

bool AudioOutput::push_paced(const std::int16_t *samples, std::size_t frames) {
  converted_.clear();
  const std::size_t produced = resampler_.process(samples, frames, converted_);
  const std::size_t accepted = ring_.write(converted_.data(), produced);
  overflow_frames_ += produced - accepted;
  return true;
}

bool AudioOutput::push_blocking(const std::int16_t *samples, std::size_t frames) {
  const std::uint64_t deadline = SDL_GetTicks() + 5000;
  while (device_queued_frames() >= kBlockingBlocks * frames_per_buffer_) {
    if (pump_messages_ && !pump_messages_()) return false;
    if (SDL_GetTicks() >= deadline) throw std::runtime_error("SDL audio device stalled for five seconds");
    ++waits_;
    GT4_PERF_SCOPE(gt4::perf::Category::audio_wait);
    SDL_Delay(1);
  }
  if (device_queued_frames() == 0 && submitted_ != 0) ++empty_queue_;
  if (!SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * kBytesPerFrame)))
    throw std::runtime_error(std::string("SDL_PutAudioStreamData failed: ") + SDL_GetError());
  submitted_ += frames;
  peak_ = std::max(peak_, device_queued_frames() / frames_per_buffer_);
  return true;
}

bool AudioOutput::push(const std::int16_t *samples, std::size_t frames) {
  GT4_PERF_SCOPE(gt4::perf::Category::audio_push);
  if (closed_ || (!stream_ && !silent_) || (frames && !samples))
    throw std::runtime_error("Invalid audio sink input/lifetime");
  received_ += frames;
  if (silent_) return true;
#if defined(GT4_PERF_PROBE) && !defined(GT4_PERF_FRAME_ONLY)
  gt4::perf::Probe::shared().audio(frames, 0);
#endif
  if (mode_ == gt4::pacing::Mode::paced) return push_paced(samples, frames);
  return push_blocking(samples, frames);
}

void AudioOutput::pump() {
  if (mode_ != gt4::pacing::Mode::paced || !stream_) return;
  if (!started_ && ring_.size() >= target_latency_frames_) started_ = true;
  if (!started_) return;
  const std::size_t device = device_queued_frames();
  // Nothing queued on the device after audio was submitted: the device starved.
  if (device == 0 && submitted_ != 0) ++underruns_;
  completed_ = submitted_ - std::min<std::uint64_t>(submitted_, device);
  // The controller sees the whole queue: device plus ring.
  resampler_.set_ratio(controller_.update(static_cast<double>(device + ring_.size())));
  // Submit only while the device stays within the target: the excess waits in the ring, where the controller sees it.
  std::size_t queued = device;
  while (ring_.size() >= frames_per_buffer_ && queued + frames_per_buffer_ <= target_latency_frames_) {
    ring_.read(block_.data(), frames_per_buffer_);
    if (!SDL_PutAudioStreamData(stream_, block_.data(), static_cast<int>(frames_per_buffer_ * kBytesPerFrame)))
      throw std::runtime_error(std::string("SDL_PutAudioStreamData failed: ") + SDL_GetError());
    submitted_ += frames_per_buffer_;
    queued += frames_per_buffer_;
    peak_ = std::max(peak_, queued / frames_per_buffer_);
  }
}

std::uint64_t AudioOutput::queued_frames() {
  return device_queued_frames() + (mode_ == gt4::pacing::Mode::paced ? ring_.size() : 0);
}

void AudioOutput::close(bool drain) {
  if (stream_ != nullptr) {
    if (drain) {
      SDL_FlushAudioStream(stream_);
      const std::uint64_t deadline = SDL_GetTicks() + 2000;
      while (device_queued_frames() != 0 && SDL_GetTicks() < deadline) SDL_Delay(5);
    }
    canceled_ += device_queued_frames();
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::fprintf(stderr,
                 "[audio] received=%llu submitted=%llu completed=%llu canceled=%llu peak_blocks=%zu waits=%llu "
                 "empty_queue=%llu\n",
                 static_cast<unsigned long long>(received_), static_cast<unsigned long long>(submitted_),
                 static_cast<unsigned long long>(completed_), static_cast<unsigned long long>(canceled_), peak_,
                 static_cast<unsigned long long>(waits_), static_cast<unsigned long long>(empty_queue_));
    if (mode_ == gt4::pacing::Mode::paced)
      std::fprintf(stderr, "[audio] paced: underruns=%llu ring_overflow_frames=%llu ring_left=%zu ratio=%.6f\n",
                   static_cast<unsigned long long>(underruns_), static_cast<unsigned long long>(overflow_frames_),
                   ring_.size(), resampler_.ratio());
  }
  if (silent_ && !closed_)
    std::fprintf(stderr, "[audio] silent: received=%llu frames dropped\n", static_cast<unsigned long long>(received_));
  closed_ = true;
}
