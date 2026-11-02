#pragma once

// Pure pacing and audio-rate logic for the native host. Nothing here touches
// Windows: the fixture drives every type with a fake clock and fake device.
//
// Clock authority: the guest VBlank schedule is a wall-clock grid at the rate
// the donor uses (frame_cycles = kEeClock / 60, NTSC 60.000 Hz). Audio never
// blocks the emulation thread; instead the produced stream is resampled by a
// ratio within +-0.5 % so the device queue stays at its latency target.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace gt4::pacing {

// IN-514: how long before the VBlank's deadline the EE finished its last frame, in microseconds, negative when late, and
// kNoSlack before the first paced frame. Written and read on the EE thread only.
inline constexpr std::int32_t kNoSlack = INT32_MIN;
inline std::int32_t ee_slack_us = kNoSlack;

// IN-521: how much later the release grid should run, in microseconds, set by the presentation's latency control.
inline std::atomic<std::int32_t> grid_shift_us{0};

// TASK-082: guest VBlanks queued in the GS worker, and what the frame skip tells the presenter: a running race where a frame may
// have no drawing (hold the display when it did not change), and frames left out lately (the display queue control waits).
inline std::atomic<std::uint32_t> gs_vsyncs_queued{0};
inline std::atomic<bool> frame_skip_engaged{false};
inline std::atomic<bool> frame_skip_recent{false};

// Audio latency target: 60 ms of stereo 48 kHz frames.
inline constexpr std::size_t target_latency_frames = 2880;
inline constexpr std::size_t sample_rate_hz = 48000;
// Paced ring capacity: about 340 ms, the bound on produced-but-unsent audio.
inline constexpr std::size_t paced_ring_frames = 16384;
// Guest VBlank rate of the donor (frame_cycles = kEeClock / 60).
inline constexpr std::uint64_t guest_vblank_rate = 60;

// GT4_PACING: unset or "paced" selects the paced clock; "audio" keeps the
// blocking WinMM path for A/B. Any other value throws std::invalid_argument.
enum class Mode { paced, audio };
Mode parse_mode(const char *value);
const char *mode_name(Mode mode);

// GT4_PRESENT_MODE: unset or "fifo" (default), "mailbox", "immediate".
// Any other value throws std::invalid_argument.
enum class PresentMode { fifo, mailbox, immediate };
PresentMode parse_present_mode(const char *value);
const char *present_mode_name(PresentMode mode);
// The mode to use on a surface that may not have the wanted one. FIFO always exists (Vulkan requires it). Mailbox and
// immediate stand in for each other (both are "do not wait for vsync"), then FIFO.
PresentMode available_present_mode(PresentMode wanted, bool mailbox_available, bool immediate_available);

// Release schedule on a clock with `frequency` ticks per second. The period is
// frequency / rate ticks, kept exact by storing the deadline scaled by `rate`,
// so 10,000 frames accumulate no drift.
class FramePacer {
public:
  // Lateness tolerance: frequency / late_tolerance_divisor ticks (1 ms).
  static constexpr std::uint64_t late_tolerance_divisor = 1000;

  FramePacer(std::uint64_t frequency, std::uint64_t rate);

  // Starts the grid: the first frame is due at `now`.
  void anchor(std::int64_t now);
  // IN-521: moves the grid by this many ticks; positive is later.
  void shift(std::int64_t ticks);
  // TASK-082: behind by this many periods before the grid restarts (default 1). With frames left out the host catches up, so a
  // heavy frame must not lose its time for good: the game keeps its speed.
  void set_resync_periods(std::uint32_t periods) { resync_periods_ = periods; }
  bool anchored() const { return anchored_; }
  // Deadline of the next release, in clock ticks (floor of the scaled value).
  std::int64_t deadline() const;
  // Ticks until the deadline, zero when the frame is due.
  std::int64_t remaining(std::int64_t now) const;
  // Records a release at `now` and advances to the next grid point. A frame
  // released more than one millisecond after its deadline counts as late
  // (late_frames); smaller lateness is jitter and is not counted. If the host
  // is behind by a full period or more, the grid restarts one period after
  // `now`: missed frames are not replayed, so the guest never runs faster than
  // the clock. Returns true when the release was late.
  bool release(std::int64_t now);
  std::uint64_t late_frames() const { return late_frames_; }
  std::uint64_t resyncs() const { return resyncs_; }

private:
  std::uint64_t frequency_;
  std::uint64_t rate_;
  bool anchored_ = false;
  std::int64_t scaled_deadline_ = 0;
  std::uint64_t late_frames_ = 0;
  std::uint64_t resyncs_ = 0;
  std::uint32_t resync_periods_ = 1;
};

// Linear resampler for interleaved stereo int16 frames. `ratio` is output
// frames per input frame. At ratio 1 the concatenated output equals the
// concatenated input bit for bit; one frame stays buffered, so each call
// returns about one frame less than its input until the next call.
class LinearResampler {
public:
  static constexpr double min_ratio = 0.995;
  static constexpr double max_ratio = 1.005;

  // Throws std::invalid_argument outside [min_ratio, max_ratio].
  void set_ratio(double ratio);
  double ratio() const { return ratio_; }
  // Appends the resampled stereo frames to `output`; returns frames appended.
  std::size_t process(const std::int16_t *input, std::size_t frames,
                      std::vector<std::int16_t> &output);

private:
  std::int16_t sample(std::size_t index, int channel,
                      const std::int16_t *input) const;
  double ratio_ = 1.0;
  // Position of the next output, relative to the previous input frame (index 0).
  double position_ = 1.0;
  std::int16_t previous_[2] = {0, 0};
};

// Proportional-integral control of the resampling ratio from the audio queue
// error. A positive error (queue below target) raises the ratio so more audio
// is produced per guest frame. Time constant about 2 s at the default gains.
struct ControllerGains {
  double proportional = 0.03;
  double integral = 0.002;
  double limit = 0.005;
};

class RateController {
public:
  RateController(double target_frames, double update_seconds,
                 ControllerGains gains = {});
  // `queued_frames` is device queue plus ring. Returns the new ratio.
  double update(double queued_frames);
  double ratio() const { return ratio_; }
  double target_frames() const { return target_frames_; }

private:
  double target_frames_;
  double update_seconds_;
  ControllerGains gains_;
  double integral_ = 0.0;
  double ratio_ = 1.0;
};

// Fixed-capacity FIFO of interleaved stereo frames.
class FrameRing {
public:
  explicit FrameRing(std::size_t capacity_frames);
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }
  // Copies as many frames as fit; returns the number accepted.
  std::size_t write(const std::int16_t *frames, std::size_t count);
  // Copies up to `count` frames out; returns the number read.
  std::size_t read(std::int16_t *frames, std::size_t count);

private:
  std::vector<std::int16_t> storage_;
  std::size_t capacity_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

// VBlank wall-interval histogram. Buckets: <12, 12-15, 15-16, 16-17, 17-18,
// 18-20, 20-25, 25-30, 30-35, >=35 ms.
class IntervalHistogram {
public:
  static constexpr std::size_t bucket_count = 10;
  void add(std::int64_t microseconds);
  std::uint64_t count(std::size_t bucket) const { return counts_[bucket]; }
  std::uint64_t total() const { return total_; }
  double mean_ms() const;
  static const char *label(std::size_t bucket);
  void reset();

private:
  std::uint64_t counts_[bucket_count]{};
  std::uint64_t total_ = 0;
  double sum_us_ = 0.0;
};

// Bounded sample window with min, median and max.
class SampleWindow {
public:
  static constexpr std::size_t capacity = 4096;
  void add(double value);
  std::size_t size() const { return size_; }
  // Requires size() > 0.
  double minimum() const;
  double median() const;
  double maximum() const;
  void reset();

private:
  double samples_[capacity]{};
  std::size_t size_ = 0;
  std::size_t next_ = 0;
};

// Session-cumulative counters owned by the host. The F8 report prints the
// difference since the previous F7 (or report) next to the session totals.
struct Counters {
  std::uint64_t late_frames = 0;
  std::uint64_t audio_underruns = 0;
  std::uint64_t ring_overflow_frames = 0;
  // Counts accrued after `earlier`; each field of `earlier` must not exceed this one.
  Counters since(const Counters &earlier) const;
};

// Diagnostics for the F8 and periodic report. Print with format(), then reset().
class Diagnostics {
public:
  // Records a release at `ticks`; the interval is measured from the previous release.
  void record_release(std::int64_t ticks, std::uint64_t frequency);
  void record_queue_frames(std::uint64_t frames);
  void record_ratio(double ratio);
  // `interval` is the counter difference for this report; `session` the totals.
  void format(std::FILE *output, Mode mode, const Counters &interval,
              const Counters &session) const;
  void reset();

private:
  IntervalHistogram intervals_;
  SampleWindow queue_;
  SampleWindow ratios_;
  std::int64_t last_release_ = 0;
  bool has_last_release_ = false;
};

} // namespace gt4::pacing
