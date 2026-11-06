#include "dfp_haptics.hpp"

#include <algorithm>
#include <cmath>

namespace gt4::wheel {

namespace {

std::int16_t signed_level(double value) {
  return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
}

std::uint16_t unsigned_level(double value) {
  return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * 65535.0));
}

// A condition effect (spring, damper, friction). `gain` scales the coefficients only; saturation, centre and dead band are
// positions and limits, not strengths.
EffectParams condition(double left, double right, double gain, double center, double deadband, double saturation) {
  EffectParams params;
  params.left_coeff = signed_level(left * gain);
  params.right_coeff = signed_level(right * gain);
  params.left_sat = params.right_sat = unsigned_level(saturation);
  params.center = signed_level(center);
  params.deadband = unsigned_level(deadband / 2.0); // DfpForces gives the band across the full travel (0..2)
  return params;
}

} // namespace

const char *effect_kind_name(EffectKind kind) {
  switch (kind) {
  case EffectKind::constant:
    return "constant";
  case EffectKind::spring:
    return "spring";
  case EffectKind::damper:
    return "damper";
  case EffectKind::friction:
    return "friction";
  }
  return "unknown";
}

EffectPlan plan_dfp_effects(const gt4::usb::DfpForces &forces, const FfbConfig &config) {
  const double gain = std::clamp(config.strength, 0.0, 2.0);
  EffectPlan plan{};
  // Constant: the game's aligning force (GT4 sends it as a variable force on slot 0 while racing).
  plan[static_cast<std::size_t>(EffectKind::constant)].level =
      signed_level((config.invert ? -forces.constant : forces.constant) * gain);
  // Spring: the game's spring; without one, the DFP's own default centring spring while it is on (wheels such as the
  // MOZA R5 have no device autocentre to stand in for it). TASK-192: both are centring springs and are off with
  // [ffb] centering_spring = "off" (the default); the saturation-zero condition sends no force and the effect idles.
  EffectParams &spring = plan[static_cast<std::size_t>(EffectKind::spring)];
  if (!config.centering_spring)
    spring = condition(0.0, 0.0, gain, 0.0, 0.0, 0.0);
  else if (forces.spring)
    spring = condition(forces.spring_left, forces.spring_right, gain, forces.spring_center, forces.spring_deadband,
                       forces.spring_saturation);
  else if (forces.autocenter)
    spring = condition(forces.autocenter_strength, forces.autocenter_strength, gain, 0.0, 0.0, 1.0);
  else
    spring = condition(0.0, 0.0, gain, 0.0, 0.0, 0.0);
  // Damper: the game's damper, at least the configured floor. The floor is the wheel's own stabiliser, applied by its
  // firmware with no game latency (a geared DFP had some of this from its own friction, a direct-drive base has almost
  // none), so it does not follow the strength; it is off only when the strength is zero.
  const double floor = gain > 0.0 ? std::clamp(config.damper, 0.0, 1.0) : 0.0;
  const double damper_left = std::max(forces.damper ? forces.damper_left : 0.0, floor);
  const double damper_right = std::max(forces.damper ? forces.damper_right : 0.0, floor);
  const double damper_saturation = forces.damper ? std::max(forces.damper_saturation, floor > 0.0 ? 1.0 : 0.0) : 1.0;
  plan[static_cast<std::size_t>(EffectKind::damper)] =
      condition(damper_left, damper_right, 1.0, 0.0, 0.0, damper_saturation);
  // Friction: the game's friction (GT4 sets it on slot 1 in menus).
  plan[static_cast<std::size_t>(EffectKind::friction)] =
      condition(forces.friction ? forces.friction_coefficient : 0.0, forces.friction ? forces.friction_coefficient : 0.0,
                gain, 0.0, 0.0, forces.friction_saturation);
  return plan;
}

// ------------------------------------------------------------------------------------------------------------ player

void DfpHapticPlayer::apply(const EffectPlan &plan, bool active) {
  if (released_)
    return;
  if (!active) {
    stop();
    return;
  }
  if (!created_) {
    // Created once with the current parameters, so the first start needs no update.
    for (std::size_t index = 0; index < effect_kind_count; ++index) {
      supported_[index] = sink_.create(static_cast<EffectKind>(index), plan[index]);
      sent_[index] = plan[index];
    }
    created_ = true;
  } else {
    // Only the effects whose parameters changed: while racing that is the constant force alone.
    for (std::size_t index = 0; index < effect_kind_count; ++index) {
      if (!supported_[index] || plan[index] == sent_[index])
        continue;
      if (sink_.update(static_cast<EffectKind>(index), plan[index])) {
        sent_[index] = plan[index];
        ++updates_sent_;
      }
    }
  }
  if (running_ || std::none_of(supported_.begin(), supported_.end(), [](bool on) { return on; }))
    return;
  sink_.set_gain(100);
  for (std::size_t index = 0; index < effect_kind_count; ++index)
    if (supported_[index])
      sink_.run(static_cast<EffectKind>(index));
  running_ = true;
  ++starts_;
}

void DfpHapticPlayer::stop() {
  if (!running_)
    return;
  sink_.stop_all();
  sink_.set_gain(0);
  running_ = false;
}

void DfpHapticPlayer::release() {
  if (released_)
    return;
  stop();
  sink_.release();
  released_ = true;
}

// ------------------------------------------------------------------------------------------------------------ thread

DfpHapticThread::DfpHapticThread(FfbConfig config, HapticSinkFactory factory, bool threaded)
    : config_(config), factory_(std::move(factory)), threaded_(threaded) {
  if (threaded_)
    thread_ = std::thread([this] { run(); });
}

DfpHapticThread::~DfpHapticThread() {
  {
    std::unique_lock lock(mutex_);
    quit_ = true;
    requested_device_ = nullptr;
    device_request_ = true;
    ++request_;
    if (!threaded_)
      apply_locked_state(lock);
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void DfpHapticThread::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return quit_ || request_ != applied_; });
    if (request_ != applied_)
      apply_locked_state(lock);
    if (quit_ && request_ == applied_)
      return;
  }
}

// Applies the latest state; the device calls run without the lock, so callers never wait on the wheel's driver.
void DfpHapticThread::apply_locked_state(std::unique_lock<std::mutex> &lock) {
  const std::uint64_t target = request_;
  const bool device_request = device_request_;
  void *device = requested_device_;
  device_request_ = false;
  const gt4::usb::DfpForces forces = forces_;
  const bool active = active_ && !quit_;
  lock.unlock();
  if (device_request) {
    close_device();
    if (device)
      open_device(device);
  }
  if (player_)
    player_->apply(plan_dfp_effects(forces, config_), active && config_.enabled);
  lock.lock();
  applied_ = std::max(applied_, target);
  stats_.device_open = player_ != nullptr;
  if (player_ && player_->running() && forces.revision != played_revision_) {
    played_revision_ = forces.revision;
    ++stats_.forces_played;
  }
  if (player_) {
    stats_.updates_sent = player_->updates_sent();
    stats_.starts = player_->starts();
    stats_.effects_supported = false;
    for (std::size_t index = 0; index < effect_kind_count; ++index)
      stats_.effects_supported = stats_.effects_supported || player_->supported(static_cast<EffectKind>(index));
  }
  done_.notify_all();
}

void DfpHapticThread::open_device(void *device) {
  if (!factory_)
    return;
  sink_ = factory_(device);
  if (sink_)
    player_ = std::make_unique<DfpHapticPlayer>(*sink_);
}

void DfpHapticThread::close_device() {
  if (player_)
    player_->release();
  player_.reset();
  sink_.reset();
}

void DfpHapticThread::attach(void *device) {
  std::unique_lock lock(mutex_);
  requested_device_ = device;
  device_request_ = true;
  const std::uint64_t mine = ++request_;
  if (!threaded_) {
    apply_locked_state(lock);
    return;
  }
  wake_.notify_all();
  done_.wait(lock, [&] { return applied_ >= mine; });
}

void DfpHapticThread::detach() { attach(nullptr); }

void DfpHapticThread::set_forces(const gt4::usb::DfpForces &forces) {
  std::unique_lock lock(mutex_);
  forces_ = forces;
  ++stats_.force_sets;
  ++request_;
  if (!threaded_) {
    apply_locked_state(lock);
    return;
  }
  lock.unlock();
  wake_.notify_all();
}

void DfpHapticThread::set_active(bool active) {
  std::unique_lock lock(mutex_);
  if (active_ == active)
    return;
  active_ = active;
  ++request_;
  if (!threaded_) {
    apply_locked_state(lock);
    return;
  }
  lock.unlock();
  wake_.notify_all();
}

void DfpHapticThread::wait_idle() {
  if (!threaded_)
    return;
  std::unique_lock lock(mutex_);
  const std::uint64_t target = request_;
  done_.wait(lock, [&] { return applied_ >= target; });
}

DfpHapticThread::Stats DfpHapticThread::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

} // namespace gt4::wheel
