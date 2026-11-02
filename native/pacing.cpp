#include "pacing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gt4::pacing {
namespace {
double clamp_value(double value, double low, double high) {
  return std::min(std::max(value, low), high);
}

std::int16_t to_sample(double value) {
  const double rounded = std::round(value);
  return static_cast<std::int16_t>(std::clamp(rounded, -32768.0, 32767.0));
}
} // namespace

Mode parse_mode(const char *value) {
  if (!value || std::strcmp(value, "paced") == 0)
    return Mode::paced;
  if (std::strcmp(value, "audio") == 0)
    return Mode::audio;
  throw std::invalid_argument("GT4_PACING must be paced or audio");
}

const char *mode_name(Mode mode) {
  return mode == Mode::paced ? "paced" : "audio";
}

PresentMode parse_present_mode(const char *value) {
  if (!value || std::strcmp(value, "fifo") == 0)
    return PresentMode::fifo;
  if (std::strcmp(value, "mailbox") == 0)
    return PresentMode::mailbox;
  if (std::strcmp(value, "immediate") == 0)
    return PresentMode::immediate;
  throw std::invalid_argument(
      "GT4_PRESENT_MODE must be fifo, mailbox or immediate");
}

PresentMode available_present_mode(PresentMode wanted, bool mailbox_available, bool immediate_available) {
  if (wanted == PresentMode::fifo) return PresentMode::fifo;
  const bool wanted_available = wanted == PresentMode::mailbox ? mailbox_available : immediate_available;
  if (wanted_available) return wanted;
  if (wanted == PresentMode::mailbox && immediate_available) return PresentMode::immediate;
  if (wanted == PresentMode::immediate && mailbox_available) return PresentMode::mailbox;
  return PresentMode::fifo;
}

const char *present_mode_name(PresentMode mode) {
  switch (mode) {
  case PresentMode::fifo: return "fifo";
  case PresentMode::mailbox: return "mailbox";
  case PresentMode::immediate: return "immediate";
  }
  return "unknown";
}

FramePacer::FramePacer(std::uint64_t frequency, std::uint64_t rate)
    : frequency_(frequency), rate_(rate) {
  if (!frequency_ || !rate_)
    throw std::invalid_argument("FramePacer needs a nonzero clock and rate");
}

void FramePacer::anchor(std::int64_t now) {
  scaled_deadline_ = now * static_cast<std::int64_t>(rate_);
  anchored_ = true;
}

void FramePacer::shift(std::int64_t ticks) {
  scaled_deadline_ += ticks * static_cast<std::int64_t>(rate_);
}

std::int64_t FramePacer::deadline() const {
  return scaled_deadline_ / static_cast<std::int64_t>(rate_);
}

std::int64_t FramePacer::remaining(std::int64_t now) const {
  return std::max<std::int64_t>(0, deadline() - now);
}

bool FramePacer::release(std::int64_t now) {
  if (!anchored_)
    anchor(now);
  // A release within one millisecond of its deadline is scheduling jitter, not a late frame.
  const bool late = now > deadline() + static_cast<std::int64_t>(frequency_ / late_tolerance_divisor);
  if (late)
    ++late_frames_;
  // One period in scaled units is exactly frequency_ (period = frequency / rate).
  scaled_deadline_ += static_cast<std::int64_t>(frequency_);
  if (now > deadline()) {
    // Behind by a full period: restart the grid one period after now.
    scaled_deadline_ = now * static_cast<std::int64_t>(rate_) +
                       static_cast<std::int64_t>(frequency_);
    ++resyncs_;
  }
  return late;
}

void LinearResampler::set_ratio(double ratio) {
  // The controller produces 1 - limit exactly, which may round one ulp outside the bound.
  constexpr double rounding_tolerance = 1e-9;
  if (!(ratio >= min_ratio - rounding_tolerance && ratio <= max_ratio + rounding_tolerance))
    throw std::invalid_argument("resampler ratio outside the bounded range");
  ratio_ = clamp_value(ratio, min_ratio, max_ratio);
}

std::int16_t LinearResampler::sample(std::size_t index, int channel,
                                     const std::int16_t *input) const {
  // Index 0 is the previous block's last frame; index k is input frame k-1.
  if (index == 0)
    return previous_[channel];
  return input[(index - 1) * 2 + static_cast<std::size_t>(channel)];
}

std::size_t LinearResampler::process(const std::int16_t *input,
                                     std::size_t frames,
                                     std::vector<std::int16_t> &output) {
  const double step = 1.0 / ratio_;
  std::size_t emitted = 0;
  while (position_ < static_cast<double>(frames)) {
    const auto index = static_cast<std::size_t>(position_);
    const double fraction = position_ - static_cast<double>(index);
    for (int channel = 0; channel < 2; ++channel) {
      const double first = sample(index, channel, input);
      const double second = sample(index + 1, channel, input);
      // fraction == 0 gives first + (second - first) * 0, exactly first.
      output.push_back(to_sample(first + (second - first) * fraction));
    }
    position_ += step;
    ++emitted;
  }
  position_ -= static_cast<double>(frames);
  if (frames) {
    previous_[0] = input[(frames - 1) * 2];
    previous_[1] = input[(frames - 1) * 2 + 1];
  }
  return emitted;
}

RateController::RateController(double target_frames, double update_seconds,
                               ControllerGains gains)
    : target_frames_(target_frames), update_seconds_(update_seconds),
      gains_(gains) {
  if (!(target_frames_ > 0.0) || !(update_seconds_ > 0.0) ||
      !(gains_.integral > 0.0) || !(gains_.limit > 0.0))
    throw std::invalid_argument("RateController needs positive parameters");
}

double RateController::update(double queued_frames) {
  const double error =
      clamp_value((target_frames_ - queued_frames) / target_frames_, -1.0, 1.0);
  // Anti-windup: the integral term alone may not exceed the ratio limit.
  const double integral_bound = gains_.limit / gains_.integral;
  integral_ = clamp_value(integral_ + error * update_seconds_, -integral_bound,
                          integral_bound);
  const double correction =
      clamp_value(gains_.proportional * error + gains_.integral * integral_,
                  -gains_.limit, gains_.limit);
  ratio_ = 1.0 + correction;
  return ratio_;
}

FrameRing::FrameRing(std::size_t capacity_frames)
    : storage_(capacity_frames * 2), capacity_(capacity_frames) {
  if (!capacity_frames)
    throw std::invalid_argument("FrameRing needs a nonzero capacity");
}

std::size_t FrameRing::write(const std::int16_t *frames, std::size_t count) {
  const std::size_t accepted = std::min(count, capacity_ - size_);
  for (std::size_t frame = 0; frame < accepted; ++frame) {
    const std::size_t slot = (head_ + size_ + frame) % capacity_;
    storage_[slot * 2] = frames[frame * 2];
    storage_[slot * 2 + 1] = frames[frame * 2 + 1];
  }
  size_ += accepted;
  return accepted;
}

std::size_t FrameRing::read(std::int16_t *frames, std::size_t count) {
  const std::size_t taken = std::min(count, size_);
  for (std::size_t frame = 0; frame < taken; ++frame) {
    const std::size_t slot = (head_ + frame) % capacity_;
    frames[frame * 2] = storage_[slot * 2];
    frames[frame * 2 + 1] = storage_[slot * 2 + 1];
  }
  head_ = (head_ + taken) % capacity_;
  size_ -= taken;
  return taken;
}

void IntervalHistogram::add(std::int64_t microseconds) {
  // Upper edges in microseconds; the last bucket is open-ended.
  static constexpr std::int64_t edges[bucket_count - 1] = {
      12000, 15000, 16000, 17000, 18000, 20000, 25000, 30000, 35000};
  std::size_t bucket = 0;
  while (bucket < bucket_count - 1 && microseconds >= edges[bucket])
    ++bucket;
  ++counts_[bucket];
  ++total_;
  sum_us_ += static_cast<double>(microseconds);
}

double IntervalHistogram::mean_ms() const {
  return total_ ? sum_us_ / static_cast<double>(total_) / 1000.0 : 0.0;
}

const char *IntervalHistogram::label(std::size_t bucket) {
  static const char *labels[bucket_count] = {
      "<12", "12-15", "15-16", "16-17", "17-18",
      "18-20", "20-25", "25-30", "30-35", ">=35"};
  return labels[bucket];
}

void IntervalHistogram::reset() {
  std::fill(std::begin(counts_), std::end(counts_), 0);
  total_ = 0;
  sum_us_ = 0.0;
}

void SampleWindow::add(double value) {
  samples_[next_] = value;
  next_ = (next_ + 1) % capacity;
  size_ = std::min(size_ + 1, capacity);
}

double SampleWindow::minimum() const {
  return *std::min_element(samples_, samples_ + size_);
}

double SampleWindow::maximum() const {
  return *std::max_element(samples_, samples_ + size_);
}

double SampleWindow::median() const {
  std::vector<double> sorted(samples_, samples_ + size_);
  const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(size_ / 2);
  std::nth_element(sorted.begin(), middle, sorted.end());
  if (size_ % 2)
    return *middle;
  const double upper = *middle;
  const double lower = *std::max_element(sorted.begin(), middle);
  return (lower + upper) / 2.0;
}

void SampleWindow::reset() {
  size_ = 0;
  next_ = 0;
}

void Diagnostics::record_release(std::int64_t ticks, std::uint64_t frequency) {
  if (has_last_release_ && frequency) {
    const std::int64_t delta = ticks - last_release_;
    // Ticks to microseconds without overflow for intervals under a minute.
    intervals_.add(delta * 1000000 / static_cast<std::int64_t>(frequency));
  }
  last_release_ = ticks;
  has_last_release_ = true;
}

void Diagnostics::record_queue_frames(std::uint64_t frames) {
  queue_.add(static_cast<double>(frames));
}

void Diagnostics::record_ratio(double ratio) { ratios_.add(ratio); }

Counters Counters::since(const Counters &earlier) const {
  return {late_frames - earlier.late_frames,
          audio_underruns - earlier.audio_underruns,
          ring_overflow_frames - earlier.ring_overflow_frames};
}

void Diagnostics::format(std::FILE *output, Mode mode, const Counters &interval,
                         const Counters &session) const {
  std::fprintf(output, "[pacing] mode=%s vblank_intervals=%llu mean=%.3f ms buckets(ms):",
               mode_name(mode), static_cast<unsigned long long>(intervals_.total()),
               intervals_.mean_ms());
  for (std::size_t bucket = 0; bucket < IntervalHistogram::bucket_count; ++bucket)
    std::fprintf(output, " %s=%llu", IntervalHistogram::label(bucket),
                 static_cast<unsigned long long>(intervals_.count(bucket)));
  std::fputc('\n', output);
  if (queue_.size()) {
    std::fprintf(output, "[pacing] audio queue frames min=%.0f median=%.0f max=%.0f (target %zu) samples=%zu\n",
                 queue_.minimum(), queue_.median(), queue_.maximum(),
                 target_latency_frames, queue_.size());
  } else {
    std::fputs("[pacing] audio queue frames: no samples\n", output);
  }
  if (ratios_.size())
    std::fprintf(output, "[pacing] resample ratio min=%.6f median=%.6f max=%.6f\n",
                 ratios_.minimum(), ratios_.median(), ratios_.maximum());
  std::fprintf(output, "[pacing] interval late_frames=%llu audio_underruns=%llu ring_overflow_frames=%llu\n",
               static_cast<unsigned long long>(interval.late_frames),
               static_cast<unsigned long long>(interval.audio_underruns),
               static_cast<unsigned long long>(interval.ring_overflow_frames));
  std::fprintf(output, "[pacing] session late_frames=%llu audio_underruns=%llu ring_overflow_frames=%llu\n",
               static_cast<unsigned long long>(session.late_frames),
               static_cast<unsigned long long>(session.audio_underruns),
               static_cast<unsigned long long>(session.ring_overflow_frames));
  std::fflush(output);
}

void Diagnostics::reset() {
  intervals_.reset();
  queue_.reset();
  ratios_.reset();
}

} // namespace gt4::pacing
