#include "perf_probe.hpp"
#include "perf_clock.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#ifdef GT4_PERF_FRAME_ONLY
static_assert(std::is_empty_v<gt4::perf::Scope>, "control must have no subsystem probe state");
static_assert(std::is_trivially_destructible_v<gt4::perf::Scope>, "control must have no timed returns");
#endif

namespace {
std::uint64_t tick = 0, cpu_tick = 0, clock_calls = 0;
std::uint64_t clock() { ++clock_calls; return tick; }
std::uint64_t cpu() { return cpu_tick; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::string contents(std::FILE *stream) {
  std::fflush(stream);
  std::rewind(stream);
  std::string result;
  char buffer[1024];
  while (std::fgets(buffer, sizeof buffer, stream)) result += buffer;
  std::fseek(stream, 0, SEEK_END);
  return result;
}
void check_manual_and_boundaries(std::FILE *stream) {
  using namespace gt4::perf;
  tick = cpu_tick = 0;
  Probe probe({clock, cpu}, true, stream);
  const auto ee = probe.enter(Category::ee_dispatch);
  probe.frame_boundary({0, 0, 0});
  probe.request(Request::start, true);
  tick = 1000; probe.frame_boundary({100, 1, 2});
  require(probe.frames() == 1, "repeat must not reset");
  probe.request(Request::start);
  require(probe.frames() == 1, "request is only latched");
  tick = 2000; probe.frame_boundary({200, 2, 3});
  require(probe.frames() == 0, "F7 starts at boundary");
  const auto iop = probe.enter(Category::iop_run);
  tick = 3000; probe.leave(iop);
  const auto vif = probe.enter(Category::vif_gif);
  tick = 4000; const auto vu = probe.enter(Category::vu_interpret);
  tick = 5000; probe.leave(vu);
  tick = 6000; probe.leave(vif);
  // One manual interval longer than the automatic five-second cadence.
  for (int frame = 1; frame <= 6; ++frame) {
    tick = 2000 + std::uint64_t(frame) * 1000000;
    cpu_tick = std::uint64_t(frame) * 1000;
    probe.presentation(640ull * 448 * 4);
    probe.bytes(false, 10); probe.bytes(true, 20);
    probe.audio(800, 800, 1); probe.dispatch(2);
    probe.frame_boundary({200 + std::uint64_t(frame) * 4915200, 2 + std::uint64_t(frame), 3 + std::uint64_t(frame)});
  }
  require(probe.frames() == 6 && probe.interval().elapsed_us == 6000000,
          "periodic reports must not reset manual interval");
  require(contents(stream).find("[perf] periodic:") == std::string::npos,
          "manual interval must suppress automatic reports");
  probe.request(Request::report);
  tick += 1000000; cpu_tick += 1000;
  probe.frame_boundary({200 + 7ull * 4915200, 9, 10});
  const auto text = contents(stream);
  require(text.find("7 VBlank samples in 7.000s") != std::string::npos, "whole manual duration");
  require(text.find("exclusive wall") != std::string::npos && text.find("DO NOT SUM") != std::string::npos,
          "exclusive and inclusive labels");
  require(text.find("presented_RGBA_bytes/frame") != std::string::npos &&
          text.find("upload bytes") == std::string::npos, "accurate byte labels");
  require(text.find("measured_thread_cpu_ms/frame=1.000") != std::string::npos, "actual CPU units");
  require(probe.frames() == 0 && probe.interval().elapsed_us == 0, "report clips pending EE");
  tick += 1000; probe.leave(ee);
  require(probe.interval().elapsed_us == 1000, "old EE time must not leak");
  require(probe.valid(), "valid manual accounting");
}

void check_disable_and_fault(std::FILE *stream) {
  using namespace gt4::perf;
  tick = 0;
  Probe probe({clock, nullptr}, false, stream);
  const auto before = clock_calls;
  const auto ee = probe.enter(Category::ee_dispatch);
  tick = 1000000;
  probe.frame_boundary({100, 2, 3});
  require(clock_calls == before, "disabled scopes/frame must not read clock");
  probe.request(Request::toggle);
  probe.frame_boundary({100, 2, 3});
  tick += 1000; probe.frame_boundary({200, 3, 4});
  require(probe.frames() == 1 && probe.interval().elapsed_us == 1000, "disabled gap excluded");
  probe.request(Request::toggle);
  tick += 1000; probe.frame_boundary({300, 4, 5});
  const auto stopped = clock_calls;
  tick += 90000000;
  probe.leave(ee);
  probe.frame_boundary({0, 0, 0});
  require(clock_calls == stopped, "disabled returns must not read clock");
  probe.request(Request::start); probe.frame_boundary({300, 4, 5});
  tick += 1000; probe.frame_boundary({100, 1, 1});
  probe.request(Request::report); tick += 1000; probe.frame_boundary({200, 2, 2});
  require(contents(stream).find("guest counters reset") != std::string::npos, "counter reset is explicit");
  require(probe.valid(), "toggle accounting valid");
  std::thread other([&probe] { require(probe.enter(Category::vu_interpret) == 0, "foreign scope rejected"); });
  other.join();
  require(!probe.valid(), "foreign thread invalidates metrics");
}

void check_percentiles_and_tail(std::FILE *stream) {
  using namespace gt4::perf;
  tick = 0;
  Probe probe({clock, nullptr}, true, stream);
  probe.request(Request::start); probe.frame_boundary({});
  for (const auto period : {1000, 3000, 2000}) {
    tick += period; probe.frame_boundary({});
  }
  probe.request(Request::report); tick += 4000; probe.frame_boundary({});
  require(contents(stream).find("median 2.500 p95 4.000 p99 4.000 max 4.000") != std::string::npos,
          "nearest-rank p95/p99 and even median");
  probe.request(Request::start); probe.frame_boundary({});
  for (int index = 0; index < 4100; ++index) { tick += 1; probe.frame_boundary({}); }
  probe.request(Request::report); tick += 1; probe.frame_boundary({});
  require(contents(stream).find("samples=4096/4101") != std::string::npos, "bounded tail disclosed");
}

void check_report_and_pause_coalescing(std::FILE *stream) {
  using namespace gt4::perf;
  tick = 0;
  Probe probe({clock, nullptr}, true, stream);
  probe.request(Request::start); probe.frame_boundary({});
  tick = 1000; probe.frame_boundary({});
  const auto before = contents(stream).size();
  probe.request(Request::report); probe.request(Request::toggle);
  tick = 2000; probe.frame_boundary({});
  const auto tail = contents(stream).substr(before);
  require(tail.find("manual (F8): 2 VBlank samples in 0.002s") != std::string::npos,
          "F8 must save before same-boundary F9 pause");
  require(!probe.enabled(), "coalesced pause applied");
  probe.request(Request::toggle); tick = 5000; probe.frame_boundary({});
  tick = 6000; probe.frame_boundary({});
  const auto before_pause = contents(stream).size();
  probe.request(Request::toggle); tick = 7000; probe.frame_boundary({});
  require(contents(stream).substr(before_pause).find("diagnostics paused (F9): 2 VBlank samples") != std::string::npos,
          "F9 pause preserves unreported complete interval");
}

void check_frame_only_report(std::FILE *stream) {
  using namespace gt4::perf;
  tick = cpu_tick = 0;
  Probe probe({clock, cpu}, true, stream, false);
  const auto beginning = contents(stream).size();
  probe.request(Request::start); probe.frame_boundary({});
  tick = 1000; cpu_tick = 500; probe.frame_boundary({4915200, 10, 2});
  probe.request(Request::report);
  tick = 2000; cpu_tick = 1000; probe.frame_boundary({9830400, 20, 4});
  const auto report = contents(stream).substr(beginning);
  require(report.find("2 VBlank samples in 0.002s") != std::string::npos,
          "control retains completed frame intervals");
  require(report.find("measurement_mode=frame-only") != std::string::npos &&
          report.find("measured_thread_cpu_ms/frame=0.500") != std::string::npos,
          "control labels mode and retains actual thread CPU");
  require(report.find("guest_clock_model_speed=") != std::string::npos &&
          report.find("primitives/frame=10.0 vu1_interpreted/frame=2.0") != std::string::npos,
          "both modes retain the same boundary guest progress/workload counters");
  require(report.find("exclusive wall") == std::string::npos &&
          report.find("mapped_write_bytes") == std::string::npos &&
          report.find("Unavailable (compiled out)") != std::string::npos,
          "control must not invent absent subsystem attribution/counters");
}
void check_split_vu_report(std::FILE *stream) {
  using namespace gt4::perf;
  tick = cpu_tick = 0;
  Probe probe({clock, cpu}, true, stream, true, true);
  const auto beginning = contents(stream).size();
  probe.request(Request::start); probe.frame_boundary({});
  const auto vu0 = probe.enter(vu_interpreter_category(0));
  tick = 1000; probe.leave(vu0);
  const auto vu1 = probe.enter(vu_interpreter_category(1));
  tick = 3000; probe.leave(vu1);
  probe.request(Request::report); probe.frame_boundary({});
  const auto report = contents(stream).substr(beginning);
  require(report.find("vu0_micro_interpret=1.000 vu1_interpret=2.000") != std::string::npos &&
          report.find("vu_interpreter_total_exclusive_ms/frame=3.000") != std::string::npos,
          "separate unit times and derived total");
#ifdef GT4_VU1_AOT
  // The VU1 AOT overlay build reports generated execution as included, in its own category.
  require(report.find("vu_scope=unit-split run_fast only") != std::string::npos &&
          report.find("measurement_mode=vu-split") != std::string::npos &&
          report.find("VU0_macro_in_EE; generated_VU1_AOT_included") != std::string::npos,
          "unit scope reports generated VU1 AOT execution as included");
#else
  require(report.find("vu_scope=unit-split run_fast only") != std::string::npos &&
          report.find("measurement_mode=vu-split") != std::string::npos &&
          report.find("VU0_macro_in_EE; generated_VU1_AOT_excluded") != std::string::npos,
          "unit scope excludes macro/AOT paths explicitly");
#endif
}
}

namespace {
// TASK-134 P1d: the TSC scale gives microseconds within 1 us per second, and the clock never steps back.
void check_scope_clock() {
  using namespace gt4::perf;
  const std::uint64_t scale = tsc_scale_from(3'000'000'000ull, 10'000'000ull, 10'000'000ull);  // 3 GHz, measured 1 s
  require(scale != 0, "scale from a 3 GHz TSC");
  const std::uint64_t one_second = wall_us_from_ticks(3'000'000'000ull, scale);
  require(one_second >= 999'999 && one_second <= 1'000'001, "3e9 ticks are one second");
  const std::uint64_t one_day = wall_us_from_ticks(3'000'000'000ull * 86400ull, scale);
  require(one_day >= 86'399'990'000ull && one_day <= 86'400'010'000ull, "no overflow over a day of ticks");
  require(tsc_scale_from(0, 1, 1) == 0 && tsc_scale_from(1, 0, 1) == 0 && tsc_scale_from(1, 1, 0) == 0, "no scale from an empty interval");
  require(tsc_scale_from(1, 1, 1) == 0, "a TSC below 1 MHz gives no scale");
  std::uint64_t last = scope_wall_us();
  for (int read = 0; read < 100000; ++read) {
    const std::uint64_t now = scope_wall_us();
    require(now >= last, "the scope clock does not step back");
    last = now;
  }
  const auto steady_start = steady_wall_us();
  const auto scope_start = scope_wall_us();
  while (steady_wall_us() - steady_start < 20000) {}
  const auto scope_elapsed = scope_wall_us() - scope_start;
  require(scope_elapsed >= 19000 && scope_elapsed <= 21000, "the scope clock follows the steady clock");
}
}

int main() {
  std::FILE *stream = nullptr;
  if (tmpfile_s(&stream) != 0 || !stream) return 1;
  try {
    check_manual_and_boundaries(stream);
    check_disable_and_fault(stream);
    check_percentiles_and_tail(stream);
    check_report_and_pause_coalescing(stream);
    check_frame_only_report(stream);
    check_split_vu_report(stream);
    check_scope_clock();
    std::fclose(stream);
    std::fputs("PASS: controlled-clock performance adapter\n", stderr);
    return 0;
  } catch (const std::exception &error) {
    std::fclose(stream);
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
