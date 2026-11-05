#pragma once
// Force feedback of the native Driving Force Pro path (TASK-018, TASK-181; docs/WHEEL-NATIVE-DFP.md).
//
// The game's own force commands are decoded by native/usb_dfp.cpp into DfpForces. This file turns them into four persistent
// effects on the host wheel (constant, spring, damper, friction) and plays them through one haptic sink:
//   - plan_dfp_effects(): pure, the effect parameters for a force set and the [ffb] settings;
//   - DfpHapticPlayer: creates the effects once, then sends only the effects whose parameters changed, starts them once and
//     stops them once (focus loss, exit);
//   - DfpHapticThread: the single owner of the haptic device. The emulation thread only stores the latest force set; a small
//     thread applies it, so a slow wheel driver never stalls the game and intermediate force sets are dropped.
// No SDL here: the SDL sink lives in native/wheel.cpp, and the fixtures use a fake sink.

#include "usb_dfp.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace gt4::wheel {

// [ffb] in wheel.toml.
struct FfbConfig {
  bool enabled = true;
  double strength = 1.0; // 0.0..2.0, scales the game's forces
  double damper = 0.15;  // 0.0..1.0, the least damping the wheel always has (not scaled by strength)
  bool invert = false;   // reverses the game's constant force for wheels that turn the other way
};

enum class EffectKind : std::uint8_t { constant, spring, damper, friction };
inline constexpr std::size_t effect_kind_count = 4;
const char *effect_kind_name(EffectKind kind);

// One effect's parameters in SDL3 haptic units (signed 16-bit levels and coefficients, unsigned 16-bit saturation and
// dead band), so two equal values mean nothing has to be sent to the wheel.
struct EffectParams {
  std::int16_t level = 0;        // constant force; positive = towards the right
  std::int16_t left_coeff = 0;   // condition effects (spring, damper, friction)
  std::int16_t right_coeff = 0;
  std::uint16_t left_sat = 0;
  std::uint16_t right_sat = 0;
  std::int16_t center = 0;
  std::uint16_t deadband = 0;
  bool operator==(const EffectParams &other) const = default;
};

using EffectPlan = std::array<EffectParams, effect_kind_count>;

// The four effects for the game's current forces, scaled by the [ffb] settings.
EffectPlan plan_dfp_effects(const gt4::usb::DfpForces &forces, const FfbConfig &config);

// Where effects go. Production: SDL3 haptics on the wheel (native/wheel.cpp). Fixtures: a recording fake.
class HapticSink {
public:
  virtual ~HapticSink() = default;
  // Creates the effect stopped with these parameters; false when the device does not support it.
  virtual bool create(EffectKind kind, const EffectParams &params) = 0;
  virtual bool update(EffectKind kind, const EffectParams &params) = 0;
  virtual bool run(EffectKind kind) = 0;
  virtual void stop_all() = 0;
  virtual void set_gain(int percent) = 0; // ignored when the device has no gain control
  virtual void release() = 0;             // destroys the effects and closes the device
};

// The effect lifecycle on one sink. Not thread-safe: one owner calls it (DfpHapticThread).
class DfpHapticPlayer {
public:
  explicit DfpHapticPlayer(HapticSink &sink) : sink_(sink) {}
  // Plays `plan` while `active` (the game window has focus and force feedback is on); otherwise stops once.
  void apply(const EffectPlan &plan, bool active);
  void stop();
  void release();
  bool running() const { return running_; }
  bool supported(EffectKind kind) const { return supported_[static_cast<std::size_t>(kind)]; }
  std::uint64_t updates_sent() const { return updates_sent_; }
  std::uint64_t starts() const { return starts_; }

private:
  HapticSink &sink_;
  bool created_ = false;
  bool running_ = false;
  bool released_ = false;
  std::array<bool, effect_kind_count> supported_{};
  EffectPlan sent_{};
  std::uint64_t updates_sent_ = 0;
  std::uint64_t starts_ = 0;
};

// Opens the sink for a device (an SDL_Joystick in production; any token in the fixtures). Null when the device has no
// force feedback.
using HapticSinkFactory = std::function<std::unique_ptr<HapticSink>(void *device)>;

class DfpHapticThread {
public:
  struct Stats {
    std::uint64_t force_sets = 0;   // force sets the game sent (DfpBridge revisions seen)
    std::uint64_t forces_played = 0; // of those, the ones applied while the effects ran on a device: the game's forces
                                     // reached the wheel
    std::uint64_t updates_sent = 0; // effect parameter updates sent to the wheel
    std::uint64_t starts = 0;       // times the effects were started (first force, focus regained)
    bool device_open = false;       // a haptic device is open
    bool effects_supported = false; // it accepted at least one effect
  };

  // `threaded` = false applies everything on the calling thread (A/B switch GT4_WHEEL_HAPTIC_THREAD=0).
  DfpHapticThread(FfbConfig config, HapticSinkFactory factory, bool threaded);
  ~DfpHapticThread();
  DfpHapticThread(const DfpHapticThread &) = delete;
  DfpHapticThread &operator=(const DfpHapticThread &) = delete;

  // Opens the haptic device of `device` (closing any previous one). Returns once the device is open.
  void attach(void *device);
  // Stops and closes the device; returns once it is closed, so the caller may close the joystick afterwards.
  void detach();
  // The latest forces the game asked for; called on the emulation thread for every force command.
  void set_forces(const gt4::usb::DfpForces &forces);
  // Focus: forces play only while active. Inactive stops the effects once.
  void set_active(bool active);
  // Returns when every request made so far has been applied (fixtures, shutdown).
  void wait_idle();
  Stats stats() const;
  bool threaded() const { return threaded_; }

private:
  void run();
  void apply_locked_state(std::unique_lock<std::mutex> &lock);
  void open_device(void *device);
  void close_device();

  const FfbConfig config_;
  const HapticSinkFactory factory_;
  const bool threaded_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  // State shared with the callers (under mutex_).
  gt4::usb::DfpForces forces_{};
  bool active_ = false;
  bool quit_ = false;
  bool device_request_ = false; // attach or detach pending
  void *requested_device_ = nullptr;
  std::uint64_t request_ = 0; // bumped by every change
  std::uint64_t applied_ = 0; // the last request the owner applied
  std::uint64_t played_revision_ = 0; // the last force revision applied to running effects
  Stats stats_{};
  // Owned by the applying thread only.
  std::unique_ptr<HapticSink> sink_;
  std::unique_ptr<DfpHapticPlayer> player_;
  std::thread thread_;
};

} // namespace gt4::wheel
