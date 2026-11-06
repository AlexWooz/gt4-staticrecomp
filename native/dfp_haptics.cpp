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
  // Constant: the game's aligning force (GT4 sends it as a variable force on slot 0 while racing). It exists the whole
  // time the effects play, as PCSX2's one constant effect does; level 0 simply applies no force.
  PlannedEffect &constant = plan[static_cast<std::size_t>(EffectKind::constant)];
  constant.active = true;
  constant.params.level = signed_level((config.invert ? -forces.constant : forces.constant) * gain);
  // Spring: the game's own spring, only while the game asks for it and only with [ffb] centering_spring = "game". A spring
  // stop (`23 0B`) makes it inactive, which stops the host effect; a spring on another slot replaces these parameters
  // because the decoder hands over the newest command. There is no DFP default spring to fall back on (TASK-194).
  PlannedEffect &spring = plan[static_cast<std::size_t>(EffectKind::spring)];
  if (config.centering_spring && forces.spring) {
    spring.active = true;
    spring.params =
        condition(forces.spring_left, forces.spring_right, gain, forces.spring_center, forces.spring_deadband,
                  forces.spring_saturation);
  }
  // Damper: the game's damper, at least the configured floor. The floor is the wheel's own stabiliser, applied by its
  // firmware with no game latency (a geared DFP had some of this from its own friction, a direct-drive base has almost
  // none), so it does not follow the strength; it is off only when the strength is zero or the floor is zero. With both
  // at zero there is no damper effect at all, as in PCSX2.
  const double floor = gain > 0.0 ? std::clamp(config.damper, 0.0, 1.0) : 0.0;
  PlannedEffect &damper = plan[static_cast<std::size_t>(EffectKind::damper)];
  if (forces.damper || floor > 0.0) {
    damper.active = true;
    const double damper_left = std::max(forces.damper ? forces.damper_left : 0.0, floor);
    const double damper_right = std::max(forces.damper ? forces.damper_right : 0.0, floor);
    const double damper_saturation = forces.damper ? std::max(forces.damper_saturation, floor > 0.0 ? 1.0 : 0.0) : 1.0;
    damper.params = condition(damper_left, damper_right, 1.0, 0.0, 0.0, damper_saturation);
  }
  // Friction: the game's friction (GT4 sets it on slot 1 in menus), each side's coefficient as the bytes give it.
  PlannedEffect &friction = plan[static_cast<std::size_t>(EffectKind::friction)];
  if (forces.friction) {
    friction.active = true;
    friction.params = condition(forces.friction_left, forces.friction_right, gain, 0.0, 0.0, forces.friction_saturation);
  }
  return plan;
}

// ------------------------------------------------------------------------------------------------------------ player

bool DfpHapticPlayer::running() const {
  return std::any_of(running_.begin(), running_.end(), [](bool on) { return on; });
}

void DfpHapticPlayer::apply(const EffectPlan &plan, bool active) {
  if (released_)
    return;
  if (!active) {
    stop();
    return;
  }
  for (std::size_t index = 0; index < effect_kind_count; ++index) {
    const PlannedEffect &planned = plan[index];
    const auto kind = static_cast<EffectKind>(index);
    if (!planned.active) {
      // The game no longer asks for this effect: stop it once. Its last parameters stay, so asking again is one update.
      if (running_[index]) {
        sink_.stop(kind);
        running_[index] = false;
      }
      continue;
    }
    if (!supported_[index]) {
      // Created on first use with the current parameters, so the first run needs no update. A device that refuses it is
      // never asked again, and the game's other effects are not affected.
      if (!sink_.create(kind, planned.params))
        continue;
      supported_[index] = true;
      sent_[index] = planned;
    } else if (!(planned == sent_[index])) {
      // Only the effects whose parameters changed: while racing that is the constant force alone.
      if (sink_.update(kind, planned.params)) {
        sent_[index] = planned;
        ++updates_sent_;
      }
    }
    if (!running_[index]) {
      if (!gain_on_) {
        sink_.set_gain(100);
        gain_on_ = true;
      }
      if (sink_.run(kind)) {
        running_[index] = true;
        ++starts_;
      }
    }
  }
}

void DfpHapticPlayer::stop() {
  if (!running())
    return;
  sink_.stop_all();
  sink_.set_gain(0);
  gain_on_ = false;
  running_.fill(false);
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
