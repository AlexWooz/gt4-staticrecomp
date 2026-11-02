#define _CRT_SECURE_NO_WARNINGS
// Pure pacing fixture: frame pacer grid and stall handling on a fake clock,
// resampler identity and sine accuracy, rate-controller convergence and
// bounds, ring wraparound, VBlank histogram buckets and sample windows. No
// Windows API is used.
#include "pacing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::pacing;

void require(bool value, const std::string &label) {
  if (!value)
    throw std::runtime_error(label);
}

constexpr std::uint64_t frequency = 10000000; // QueryPerformanceFrequency-like

void test_parsers() {
  require(parse_mode(nullptr) == Mode::paced, "unset GT4_PACING is paced");
  require(parse_mode("paced") == Mode::paced, "paced token");
  require(parse_mode("audio") == Mode::audio, "audio token");
  bool refused = false;
  try {
    parse_mode("Audio");
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "unknown GT4_PACING is refused");
  require(parse_present_mode(nullptr) == PresentMode::fifo, "default present mode");
  require(parse_present_mode("mailbox") == PresentMode::mailbox, "mailbox token");
  require(parse_present_mode("immediate") == PresentMode::immediate, "immediate token");
  require(available_present_mode(PresentMode::mailbox, true, true) == PresentMode::mailbox, "mailbox when it exists");
  require(available_present_mode(PresentMode::mailbox, false, true) == PresentMode::immediate, "no mailbox: immediate (HD 620)");
  require(available_present_mode(PresentMode::mailbox, false, false) == PresentMode::fifo, "no mailbox or immediate: FIFO");
  require(available_present_mode(PresentMode::immediate, true, true) == PresentMode::immediate, "immediate when it exists");
  require(available_present_mode(PresentMode::immediate, true, false) == PresentMode::mailbox, "no immediate: mailbox");
  require(available_present_mode(PresentMode::immediate, false, false) == PresentMode::fifo, "no immediate or mailbox: FIFO");
  require(available_present_mode(PresentMode::fifo, true, true) == PresentMode::fifo, "FIFO stays FIFO");
  refused = false;
  try {
    parse_present_mode("vsync");
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "unknown GT4_PRESENT_MODE is refused");
}

// Ten thousand frames released exactly on time land on the exact rational grid.
void test_period_accuracy_no_drift() {
  FramePacer pacer(frequency, 60);
  pacer.anchor(1000);
  for (std::uint64_t frame = 0; frame < 10000; ++frame) {
    const std::int64_t expected =
        1000 + static_cast<std::int64_t>(frame * frequency / 60);
    require(pacer.deadline() == expected, "deadline on the exact grid");
    require(!pacer.release(pacer.deadline()), "exact release is not late");
  }
  require(pacer.deadline() == 1000 + static_cast<std::int64_t>(10000 * frequency / 60),
          "no drift after 10,000 frames");
  require(pacer.late_frames() == 0 && pacer.resyncs() == 0, "clean run has no late frames");
}

// TASK-082: with frames left out, a heavy frame keeps its place on the grid up to the limit.
void test_resync_periods() {
  FramePacer pacer(frequency, 60);
  const std::int64_t period = static_cast<std::int64_t>(frequency) / 60;
  pacer.anchor(0);
  pacer.set_resync_periods(4);
  pacer.release(0);
  pacer.release(period * 3);
  require(pacer.resyncs() == 0, "three periods behind must not restart a grid that allows four");
  require(pacer.deadline() == static_cast<std::int64_t>(2 * frequency / 60), "the grid must keep its place");
  pacer.release(period * 8);
  require(pacer.resyncs() == 1, "past the limit the grid restarts");
  require(pacer.deadline() == period * 8 + static_cast<std::int64_t>(frequency / 60), "a restart is one period after now");
}

// IN-521: a shift moves every later deadline by the same amount.
void test_grid_shift() {
  FramePacer pacer(frequency, 60);
  pacer.anchor(1000);
  const std::int64_t before = pacer.deadline();
  pacer.shift(1234);
  require(pacer.deadline() == before + 1234, "a shift moves the deadline later");
  pacer.shift(-1234);
  require(pacer.deadline() == before, "a shift back returns to the grid");
}

void test_remaining_and_late_frames() {
  FramePacer pacer(frequency, 60);
  pacer.anchor(0);
  require(pacer.remaining(0) == 0, "first frame due immediately");
  pacer.release(0);
  require(pacer.remaining(0) == 166666, "remaining before the second deadline");
  // Lateness within the 1 ms tolerance is jitter: released on time, not counted.
  const std::int64_t half_millisecond = 5000;
  const std::int64_t one_millisecond = 10000;
  for (int frame = 1; frame <= 3; ++frame)
    require(!pacer.release(pacer.deadline() + half_millisecond), "half a millisecond late is not late");
  require(!pacer.release(pacer.deadline() + one_millisecond), "exactly the tolerance is not late");
  require(pacer.late_frames() == 0, "jitter within the tolerance is not counted");
  // Beyond the tolerance each frame is counted late; the grid is kept (less than a period).
  for (int frame = 1; frame <= 5; ++frame) {
    const std::int64_t late_by = 20000;
    require(pacer.release(pacer.deadline() + late_by), "release 2 ms after deadline is late");
  }
  require(pacer.late_frames() == 5, "late frames counted beyond the tolerance");
  require(pacer.resyncs() == 0, "small lateness does not restart the grid");
  require(pacer.deadline() == static_cast<std::int64_t>(10 * frequency / 60),
          "grid continues from the scheduled deadline after nine releases");
}

// A stall of several periods restarts the grid one period after the release:
// no replayed frames, so the guest never runs faster than the clock.
void test_stall_restarts_grid_without_burst() {
  FramePacer pacer(frequency, 60);
  pacer.anchor(0);
  pacer.release(0);
  const std::int64_t stall_end = 5 * 166667; // five periods late
  require(pacer.release(stall_end), "stalled release is late");
  require(pacer.resyncs() == 1, "a multi-period stall resyncs once");
  require(pacer.deadline() == stall_end + 166666,
          "next deadline one period after the stalled release");
  require(pacer.remaining(stall_end) > 0, "no immediate catch-up release after a stall");
}

// Sample i of a 1 kHz sine at 48 kHz, scaled to 16-bit stereo.
std::vector<std::int16_t> sine_block(std::size_t first, std::size_t frames,
                                     double amplitude) {
  std::vector<std::int16_t> out(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    const double angle = 2.0 * 3.14159265358979323846 * 1000.0 *
                         static_cast<double>(first + i) / 48000.0;
    const auto value = static_cast<std::int16_t>(std::lround(amplitude * std::sin(angle)));
    out[i * 2] = value;
    out[i * 2 + 1] = static_cast<std::int16_t>(-value);
  }
  return out;
}

// Unity ratio is bit-identical passthrough, across arbitrary block sizes.
void test_resampler_unity_is_passthrough() {
  LinearResampler resampler;
  std::mt19937 random(7);
  std::uniform_int_distribution<int> sample(-32768, 32767);
  std::uniform_int_distribution<std::size_t> block(1, 900);
  std::vector<std::int16_t> input, output;
  for (int round = 0; round < 200; ++round) {
    const std::size_t frames = block(random);
    std::vector<std::int16_t> chunk(frames * 2);
    for (auto &value : chunk)
      value = static_cast<std::int16_t>(sample(random));
    input.insert(input.end(), chunk.begin(), chunk.end());
    resampler.process(chunk.data(), frames, output);
  }
  // One frame stays buffered at the end of every call (documented lag).
  require(output.size() + 2 == input.size(), "unity keeps one frame buffered");
  require(std::equal(output.begin(), output.end(), input.begin()),
          "unity output equals input bit for bit");
}

// Ratio 1.002 yields 0.2 % more frames, and each frame matches the sine at its
// mapped input position within linear-interpolation error.
void test_resampler_sine_accuracy() {
  for (const double ratio : {1.002, 0.998}) {
    LinearResampler resampler;
    resampler.set_ratio(ratio);
    const std::size_t total = 48000;
    const double amplitude = 16000.0;
    std::vector<std::int16_t> output;
    std::size_t consumed = 0;
    while (consumed < total) {
      const std::size_t frames = std::min<std::size_t>(800, total - consumed);
      const auto chunk = sine_block(consumed, frames, amplitude);
      resampler.process(chunk.data(), frames, output);
      consumed += frames;
    }
    const double produced = static_cast<double>(output.size() / 2);
    require(std::fabs(produced / total - ratio) < 0.001,
            "output frame count follows the ratio");
    double worst = 0.0;
    for (std::size_t k = 0; k < output.size() / 2; ++k) {
      const double input_index = static_cast<double>(k) / ratio;
      if (input_index >= static_cast<double>(total) - 2.0)
        break;
      const double expected =
          amplitude * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * input_index / 48000.0);
      worst = std::max(worst, std::fabs(output[k * 2] - expected));
    }
    require(worst < 0.005 * amplitude, "sine error within linear interpolation bound");
  }
  LinearResampler bounded;
  bool refused = false;
  try {
    bounded.set_ratio(1.006);
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "ratio outside +-0.5 % is refused");
}

// Constant device drift of 0.2 % is absorbed by the ratio, not by the queue.
void test_controller_converges() {
  RateController controller(static_cast<double>(target_latency_frames), 1.0 / 60.0);
  double queued = static_cast<double>(target_latency_frames);
  const double drift = 0.002;
  for (int update = 0; update < 3600; ++update) {
    const double ratio = controller.update(queued);
    require(ratio >= 0.995 && ratio <= 1.005, "ratio bounded");
    queued += 800.0 * ratio - 800.0 * (1.0 + drift);
  }
  require(std::fabs(queued - target_latency_frames) < 10.0, "queue converges to target");
  require(std::fabs(controller.ratio() - (1.0 + drift)) < 2e-4, "ratio learns the drift");
}

// A drift beyond the bound saturates the ratio; it never exceeds the limit.
void test_controller_saturates() {
  RateController controller(static_cast<double>(target_latency_frames), 1.0 / 60.0);
  double queued = static_cast<double>(target_latency_frames);
  for (int update = 0; update < 600; ++update) {
    const double ratio = controller.update(queued);
    require(ratio <= 1.005 + 1e-12 && ratio >= 0.995 - 1e-12, "saturated ratio bounded");
    queued += 800.0 * ratio - 800.0 * 1.01;
  }
  require(std::fabs(controller.ratio() - 1.005) < 1e-12, "beyond-bound drift saturates at +0.5 %");
}

// Jitter of +-1 % per guest frame keeps the queue far from empty.
void test_controller_no_underrun_with_jitter() {
  RateController controller(static_cast<double>(target_latency_frames), 1.0 / 60.0);
  std::mt19937 random(3);
  std::uniform_real_distribution<double> jitter(-0.01, 0.01);
  double queued = static_cast<double>(target_latency_frames);
  double lowest = queued;
  for (int update = 0; update < 3600; ++update) {
    const double ratio = controller.update(queued);
    queued += 800.0 * (1.0 + jitter(random)) * ratio - 800.0 * 1.002;
    lowest = std::min(lowest, queued);
  }
  require(lowest > 2000.0, "queue never approaches underrun under jitter");
}

void test_ring() {
  // Four frames of capacity; frames are (L, R) pairs.
  FrameRing ring(4);
  const std::int16_t first[] = {1, 2, 3, 4, 5, 6};
  require(ring.write(first, 3) == 3, "write within capacity");
  std::int16_t out[4]{};
  require(ring.read(out, 2) == 2 && out[0] == 1 && out[2] == 3, "read FIFO order");
  const std::int16_t second[] = {7, 8, 9, 10, 11, 12};
  require(ring.write(second, 3) == 3, "write wraps around the storage end");
  require(ring.size() == 4, "ring full");
  require(ring.write(second, 1) == 0, "full ring accepts nothing");
  std::int16_t rest[8]{};
  require(ring.read(rest, 4) == 4, "read the rest");
  require(rest[0] == 5 && rest[2] == 7 && rest[4] == 9 && rest[6] == 11, "wrapped order");
}

void test_histogram_buckets() {
  IntervalHistogram histogram;
  const std::int64_t samples_us[] = {11900, 12000, 15000, 16000, 16667,
                                     17000, 18000, 20000, 34999, 35000};
  for (const auto value : samples_us)
    histogram.add(value);
  require(histogram.count(0) == 1 && histogram.count(1) == 1, "under 12 and 12-15 ms");
  require(histogram.count(2) == 1 && histogram.count(3) == 2, "15-16 and 16-17 ms");
  require(histogram.count(4) == 1 && histogram.count(5) == 1, "17-18 and 18-20 ms");
  require(histogram.count(8) == 1 && histogram.count(9) == 1, "30-35 and 35 ms and over");
  require(histogram.total() == 10, "every sample counted once");
  require(std::string(IntervalHistogram::label(9)) == ">=35", "last bucket label");
  histogram.reset();
  require(histogram.total() == 0, "reset clears the histogram");
}

void test_sample_window() {
  SampleWindow window;
  for (const double value : {5.0, 1.0, 9.0})
    window.add(value);
  require(window.minimum() == 1.0 && window.maximum() == 9.0 && window.median() == 5.0,
          "odd median");
  window.add(7.0);
  require(window.median() == 6.0, "even median averages the middle pair");
  for (std::size_t i = 0; i < SampleWindow::capacity; ++i)
    window.add(100.0);
  require(window.size() == SampleWindow::capacity && window.minimum() == 100.0,
          "window keeps the newest capacity samples");
}

void test_diagnostics_format() {
  Diagnostics diagnostics;
  diagnostics.record_release(0, frequency);
  diagnostics.record_release(166667, frequency);
  diagnostics.record_queue_frames(2880);
  diagnostics.record_ratio(1.001);
  std::FILE *output = std::tmpfile();
  require(output != nullptr, "temporary output");
  const Counters session{5, 2, 7};
  const Counters previous{3, 2, 4};
  diagnostics.format(output, Mode::paced, session.since(previous), session);
  std::rewind(output);
  char line[512]{};
  require(std::fgets(line, sizeof line, output) != nullptr, "histogram line");
  require(std::string(line).find("[pacing] mode=paced vblank_intervals=1") != std::string::npos,
          "histogram line names mode and interval count");
  std::string text;
  while (std::fgets(line, sizeof line, output))
    text += line;
  require(text.find("[pacing] interval late_frames=2 audio_underruns=0 ring_overflow_frames=3") != std::string::npos,
          "interval line carries the per-report difference");
  require(text.find("[pacing] session late_frames=5 audio_underruns=2 ring_overflow_frames=7") != std::string::npos,
          "session line carries the cumulative totals");
  std::fclose(output);
}

void test_counter_interval_difference() {
  const Counters earlier{10, 4, 100};
  const Counters later{13, 4, 250};
  const Counters delta = later.since(earlier);
  require(delta.late_frames == 3 && delta.audio_underruns == 0 && delta.ring_overflow_frames == 150,
          "interval is the difference of cumulative counters");
  require(later.since(later).late_frames == 0, "identical totals give an empty interval");
}

} // namespace

int main() {
  try {
    test_parsers();
    test_period_accuracy_no_drift();
    test_grid_shift();
    test_resync_periods();
    test_remaining_and_late_frames();
    test_stall_restarts_grid_without_burst();
    test_resampler_unity_is_passthrough();
    test_resampler_sine_accuracy();
    test_controller_converges();
    test_controller_saturates();
    test_controller_no_underrun_with_jitter();
    test_ring();
    test_histogram_buckets();
    test_sample_window();
    test_diagnostics_format();
    test_counter_interval_difference();
    std::puts("pacing fixture PASS");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "pacing fixture FAIL: %s\n", error.what());
    return 1;
  }
}
