// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: the performance HUD on known inputs: the numbers, the text, the panel's rows, and what Windows gives it.
#include "hw_hud.hpp"
#include "hw_hud_system.hpp"
#include "../../perf_report_status.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace gt4::hw::hud;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  std::printf("%s  %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) ++failures;
}

bool near(double a, double b, double tolerance = 0.01) {
  return std::abs(a - b) <= tolerance;
}

std::string text_of(const Line &line) {
  std::string out;
  for (const Segment &segment : line.left) out += segment.text;
  return out;
}

// A cell of 6x10 pixels, every glyph solid except the space.
GlyphAtlas solid_atlas() {
  GlyphAtlas atlas;
  atlas.cell_width = 6;
  atlas.cell_height = 10;
  const int glyphs = GlyphAtlas::kLast - GlyphAtlas::kFirst + 1;
  atlas.coverage.assign(static_cast<std::size_t>(glyphs * 6 * 10), 255);
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 6; ++x) atlas.coverage[static_cast<std::size_t>(y * glyphs * 6 + x)] = 0;
  }
  return atlas;
}

std::uint32_t pixel(const Canvas &canvas, int x, int y) {
  return canvas.pixels()[static_cast<std::size_t>(y) * canvas.width() + x];
}

void frame_time_checks() {
  FrameTimes times;
  check(times.window(100.0).intervals == 0, "no presentation: nothing to show");
  times.note_present(100.0);
  check(times.window(100.0).intervals == 0, "one presentation is not an interval");
  // 61 presentations, 16 ms apart, with one of 48 ms: 60 intervals.
  double now = 100.0;
  for (int i = 0; i < 60; ++i) {
    now += i == 30 ? 48.0 : 16.0;
    times.note_present(now);
  }
  const FrameWindow window = times.window(now);
  check(window.intervals == 60, "the last second's intervals");
  check(near(window.frame_ms, (59 * 16.0 + 48.0) / 60.0), "the mean frame time");
  check(near(window.frame_ms_low, 16.0) && near(window.frame_ms_high, 48.0), "the shortest and longest frames");
  check(near(window.fps, 1000.0 / ((59 * 16.0 + 48.0) / 60.0)), "FPS is presentations over time");
  check(near(window.fps_low, 1000.0 / 48.0) && near(window.fps_high, 1000.0 / 16.0), "FPS low and high come from the frames");
  check(times.window(now + 5000.0).intervals == 0, "old presentations leave the window");

  std::vector<float> recent;
  times.recent_intervals(recent, 40);
  check(recent.size() == 40, "the graph's history is as long as asked");
  check(near(recent.back(), 16.0), "the newest interval comes last");
  check(near(recent[recent.size() - 30], 48.0), "the spike is at its place");

  FrameTimes wrapped;
  for (std::size_t i = 0; i < FrameTimes::kCapacity + 50; ++i) wrapped.note_present(static_cast<double>(i) * 10.0);
  wrapped.recent_intervals(recent, FrameTimes::kCapacity * 2);
  check(recent.size() == FrameTimes::kCapacity - 1, "the history stops at its capacity");
  check(std::all_of(recent.begin(), recent.end(), [](float interval) { return near(interval, 10.0); }), "the history survives wrapping");
}

std::string join(const std::vector<std::string> &parts) {
  std::string out;
  for (const std::string &part : parts) out += (out.empty() ? "" : " ") + part;
  return out;
}

bool all_fit(const std::vector<std::string> &parts, int columns) {
  return std::all_of(parts.begin(), parts.end(), [columns](const std::string &part) { return static_cast<int>(part.size()) <= columns; });
}

std::string without_spaces(const std::string &text) {
  std::string out;
  for (const char character : text) {
    if (character != ' ') out += character;
  }
  return out;
}

void name_checks() {
  check(clean_device_name("  AMD Ryzen 7 5700X 8-Core Processor            ") == "AMD Ryzen 7 5700X 8-Core Processor", "a brand string's padding is trimmed");
  check(clean_device_name("Intel(R)  Core(TM)\t i5-7200U") == "Intel(R) Core(TM) i5-7200U", "whitespace runs become one space");
  check(clean_device_name("Radeon \xC2\xAE Graphics") == "Radeon ? Graphics", "a UTF-8 sequence is one question mark");
  check(clean_device_name("").empty() && clean_device_name("   ").empty(), "an empty name stays empty");

  // The rule: never cut. The characters of the name come back in order, whatever the width, and every line fits.
  const char *names[] = {"NVIDIA GeForce RTX 3060 Laptop GPU",
                         "AMD Radeon RX 9060 XT",
                         "Intel(R) UHD Graphics 620",
                         "AMD Custom GPU 0405",
                         "AMD Ryzen 7 5700X 8-Core Processor",
                         "Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz",
                         "NVIDIA GeForce RTX 4090 Laptop GPU with Max-Q Design and a very long marketing name indeed"};
  bool never_cut = true;
  for (const char *name : names) {
    for (const int columns : {12, 20, 30, kColumns, 80}) {
      const std::vector<std::string> parts = fit_name(name, columns);
      std::string rejoined;
      for (const std::string &part : parts) rejoined += part;
      if (!all_fit(parts, columns) || without_spaces(rejoined) != without_spaces(name)) {
        std::printf("  %s at %d columns: %s\n", name, columns, join(parts).c_str());
        never_cut = false;
      }
    }
  }
  check(never_cut, "a name fits its lines and loses nothing (7 names, 5 widths)");

  // The owner's laptop showed "NVIDIA GeForce RTX 3.." The whole name is one line of the panel.
  check(fit_name("NVIDIA GeForce RTX 3060 Laptop GPU", kColumns) == std::vector<std::string>{"NVIDIA GeForce RTX 3060 Laptop GPU"},
        "the RTX 3060 Laptop GPU is one whole line");
  check(fit_name("AMD Radeon RX 9060 XT", kColumns).size() == 1 && fit_name("Intel(R) UHD Graphics 620", kColumns).size() == 1 &&
            fit_name("AMD Custom GPU 0405", kColumns).size() == 1,
        "the RX 9060 XT, the UHD 620 and the Steam Deck's GPU are one line each");
  check(fit_name("AMD Ryzen 7 5700X 8-Core Processor", kColumns).size() == 1, "the Ryzen 7 5700X is one line");
  // A name longer than a line wraps at a space; nothing is dropped.
  const std::vector<std::string> wrapped = fit_name("Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz", kColumns);
  check(wrapped.size() == 2 && wrapped[0] == "Intel(R) Core(TM) i5-7200U CPU @" && wrapped[1] == "2.50GHz", "a long name wraps at a space");
  const std::vector<std::string> narrow = fit_name("NVIDIA GeForce RTX 3060 Laptop GPU", 20);
  check(narrow.size() == 2 && narrow[0] == "NVIDIA GeForce RTX" && narrow[1] == "3060 Laptop GPU", "a narrow panel wraps the card name in two");
  const std::vector<std::string> broken = fit_name("Supercalifragilisticexpialidocious GPU", 10);
  check(broken.size() == 4 && broken[0] == "Supercalif" && broken[1] == "ragilistic" && broken[3] == "ious GPU", "a word longer than a line is split at the line's end, not cut");
  check(fit_name("", kColumns).empty(), "no name, no lines");
}

void mode_checks() {
  check(mode_from_setting("1") == Mode::full, "GT4_HW_HUD=1 is the full panel");
  check(mode_from_setting("fps") == Mode::fps_only, "GT4_HW_HUD=fps is the frame rate alone");
  check(mode_from_setting(nullptr) == Mode::off && mode_from_setting("") == Mode::off && mode_from_setting("0") == Mode::off &&
            mode_from_setting("FPS") == Mode::off && mode_from_setting("full") == Mode::off,
        "anything else is no HUD");
}

void cpu_checks() {
  const std::string brand = cpu_brand_string();
  std::printf("  this processor: \"%s\"\n", brand.c_str());
#if defined(_M_X64) || defined(__x86_64__)
  check(!brand.empty(), "an x86 processor has a brand string");
#endif
  check(brand == clean_device_name(brand), "the brand string is already clean");
  check(brand.empty() || (brand.front() != ' ' && brand.back() != ' '), "no padding at either end");
}

void report_state_checks() {
  using gt4::perf::ReportPhase;
  using gt4::perf::ReportStatus;
  ReportStatus off(false);
  off.interval_started();
  check(report_line(off, 0) == ReportLine::hidden, "report off: the line is hidden, whatever the keys do");
  off.report_saved(10);
  check(report_line(off, 10) == ReportLine::hidden, "report off: still hidden after a save");

  ReportStatus status(true);
  check(report_line(status, 0) == ReportLine::idle, "report on, nothing yet: Idle");
  status.interval_started();
  check(report_line(status, 500) == ReportLine::recording && report_line(status, 60000) == ReportLine::recording,
        "F7: Recording, as long as the interval runs");
  status.report_saved(100000);
  check(report_line(status, 100000) == ReportLine::saved, "F8: Saved at once");
  check(report_line(status, 100000 + ReportStatus::kSavedShownMs - 1) == ReportLine::saved, "Saved lasts about 3 seconds");
  check(report_line(status, 100000 + ReportStatus::kSavedShownMs) == ReportLine::idle && report_line(status, 500000) == ReportLine::idle,
        "then Idle again, with no one writing");
  check(ReportStatus::kSavedShownMs == 3000, "the 3 seconds");
  status.report_saved(200000);
  status.interval_started();
  check(report_line(status, 200100) == ReportLine::recording, "F7 during Saved starts a new interval at once");
  status.interval_ended();
  check(status.phase(0) == ReportPhase::idle, "a paused probe is Idle");
  status.report_saved(1000);
  check(report_line(status, 5) == ReportLine::saved, "a reading taken just before the save (another thread's clock) still says Saved");
  check(ReportStatus::now_ms() <= ReportStatus::now_ms() + 1, "the clock reads");

  // The writer (the probe's thread) and the reader (the presenting thread) never share a lock.
  ReportStatus shared(true);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::uint64_t now = 0;
    while (!stop) {
      shared.interval_started();
      shared.report_saved(now += 7);
    }
  });
  bool coherent = true;
  for (int i = 0; i < 20000; ++i) {
    const ReportLine line = report_line(shared, static_cast<std::uint64_t>(i));
    if (line != ReportLine::idle && line != ReportLine::recording && line != ReportLine::saved) coherent = false;
  }
  stop = true;
  writer.join();
  check(coherent, "a reader meets only the three states while the probe writes");
}

SessionFacts laptop_facts() {
  SessionFacts facts;
  facts.gpu_name = "NVIDIA GeForce RTX 3060 Laptop GPU";
  facts.cpu_name = "AMD Ryzen 7 5700X 8-Core Processor";
  facts.scale = "3";
  facts.output_width = 3440;
  facts.output_height = 1440;
  return facts;
}

void text_checks() {
  SessionFacts facts = laptop_facts();
  facts.gpu_name = "AMD Radeon RX 9060 XT";
  FrameWindow frames;
  frames.intervals = 60;
  frames.fps = 59.94;
  frames.fps_low = 58.2;
  frames.fps_high = 60.5;
  frames.frame_ms = 16.68;
  frames.frame_ms_low = 16.53;
  frames.frame_ms_high = 17.18;
  SystemReading reading;
  reading.cpu_percent = 31.25;
  reading.gpu_percent = 46.0;
  reading.memory_mb = 812.0;
  reading.video_memory_mb = 2150.0;
  reading.refresh_hz = 144;
  const std::vector<Line> lines = hud_lines(Mode::full, ReportLine::hidden, facts, frames, reading);
  check(lines.size() == 8, "eight lines: card, processor, renderer, FPS, frame, CPU, GPU, memory (no report line while the report is off)");
  if (lines.size() != 8) return;
  check(text_of(lines[0]) == "AMD Radeon RX 9060 XT" && lines[0].right.text.empty(), "the card");
  check(text_of(lines[1]) == "AMD Ryzen 7 5700X 8-Core Processor", "the processor");
  check(text_of(lines[2]) == "CoreGS 2.0  3x" && lines[2].right.text == "[3440x1440] 144Hz", "the renderer, the scale, the output and the refresh rate");
  check(text_of(lines[3]) == "FPS:    59.94 [ 58.20  60.50]", "the FPS line");
  check(text_of(lines[4]) == "Frame:  16.68 [ 16.53  17.18] ms", "the frame time line");
  check(text_of(lines[5]) == "CPU:    31.2%", "the CPU line says CPU, not EE");
  check(text_of(lines[6]) == "GPU:    46.0%", "the GPU line says GPU, not GS");
  check(text_of(lines[7]) == "Mem:   812 MB" && lines[7].right.text == "VRAM: 2.10 GB", "the memory line");
  for (const Line &line : lines) {
    check(static_cast<int>(text_of(line).size() + line.right.text.size()) <= kColumns, "the line fits the panel");
  }
  check(text_of(lines[2]).size() + 1 + lines[2].right.text.size() <= static_cast<std::size_t>(kColumns),
        "the renderer line leaves a gap before the output size");

  // The owner's laptop: the card name whole, on one line, the processor under it.
  const std::vector<Line> laptop = hud_lines(Mode::full, ReportLine::hidden, laptop_facts(), frames, reading);
  check(text_of(laptop[0]) == "NVIDIA GeForce RTX 3060 Laptop GPU", "the laptop card is not cut");

  // A very long card name and a long processor name wrap: more lines, every character there.
  facts.gpu_name = "A graphics card with a very long marketing name indeed";
  facts.cpu_name = "Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz";
  const std::vector<Line> unknown = hud_lines(Mode::full, ReportLine::hidden, facts, FrameWindow{}, SystemReading{});
  check(unknown.size() == 10, "a long card name takes two lines and a long processor name two");
  if (unknown.size() == 10) {
    check(text_of(unknown[0]) + " " + text_of(unknown[1]) == facts.gpu_name, "the long card name is whole over its lines");
    check(text_of(unknown[2]) + " " + text_of(unknown[3]) == facts.cpu_name, "the long processor name is whole over its lines");
    check(text_of(unknown[5]) == "FPS:      --" && text_of(unknown[7]) == "CPU:      --" && text_of(unknown[9]) == "Mem:   --",
          "what is not known yet shows as dashes");
    check(unknown[4].right.text == "[3440x1440]", "no refresh rate until it is known");
  }

  // Without a processor name there is no processor line.
  facts.cpu_name.clear();
  facts.gpu_name = "AMD Custom GPU 0405";
  check(hud_lines(Mode::full, ReportLine::hidden, facts, FrameWindow{}, SystemReading{}).size() == 7, "no processor name, no processor line");

  // The number of lines never depends on the readings.
  const SessionFacts fixed = laptop_facts();
  check(hud_lines(Mode::full, ReportLine::idle, fixed, FrameWindow{}, SystemReading{}).size() ==
            hud_lines(Mode::full, ReportLine::idle, fixed, frames, reading).size(),
        "the line count is the same with and without readings");
}

void report_line_checks() {
  const SessionFacts facts = laptop_facts();
  const std::size_t without = hud_lines(Mode::full, ReportLine::hidden, facts, FrameWindow{}, SystemReading{}).size();
  struct Case {
    ReportLine state;
    const char *text;
  };
  for (const Case &item : {Case{ReportLine::idle, "Report: Idle"}, Case{ReportLine::recording, "Report: Recording"}, Case{ReportLine::saved, "Report: Saved"}}) {
    const std::vector<Line> lines = hud_lines(Mode::full, item.state, facts, FrameWindow{}, SystemReading{});
    check(lines.size() == without + 1 && text_of(lines.back()) == item.text, item.text);
    check(static_cast<int>(text_of(lines.back()).size()) <= kColumns, "the report line fits the panel");
  }
  const std::vector<Line> hidden = hud_lines(Mode::full, ReportLine::hidden, facts, FrameWindow{}, SystemReading{});
  check(std::none_of(hidden.begin(), hidden.end(), [](const Line &line) { return text_of(line).rfind("Report", 0) == 0; }),
        "report off: no report line at all");
  // The three states look different, so a glance tells them apart.
  const auto colour_of = [&](ReportLine state) {
    return hud_lines(Mode::full, state, facts, FrameWindow{}, SystemReading{}).back().left.back().colour;
  };
  check(colour_of(ReportLine::idle) != colour_of(ReportLine::recording) && colour_of(ReportLine::recording) != colour_of(ReportLine::saved) &&
            colour_of(ReportLine::idle) != colour_of(ReportLine::saved),
        "Idle, Recording and Saved have three colours");
}

void fps_only_checks() {
  const SessionFacts facts = laptop_facts();
  SystemReading reading;
  reading.refresh_hz = 144;
  reading.cpu_percent = 10.0;
  FrameWindow frames;
  frames.intervals = 60;
  frames.fps = 59.94;
  const std::vector<Line> lines = hud_lines(Mode::fps_only, ReportLine::recording, facts, frames, reading);
  check(lines.size() == 1, "FPS only is one line, whatever the report does");
  if (lines.size() != 1) return;
  check(text_of(lines[0]) == " 59.9 FPS" && lines[0].right.text.empty(), "the frame rate and nothing else");
  check(static_cast<int>(text_of(lines[0]).size()) <= kFpsColumns, "the line fits the small panel");
  frames.fps = 144.04;
  check(text_of(hud_lines(Mode::fps_only, ReportLine::hidden, facts, frames, reading)[0]) == "144.0 FPS", "three digits still fit");
  check(text_of(hud_lines(Mode::fps_only, ReportLine::hidden, facts, FrameWindow{}, reading)[0]) == "  -- FPS", "dashes until the first interval");
  check(hud_lines(Mode::off, ReportLine::hidden, facts, frames, reading).empty(), "off draws nothing");

  // Its panel: narrow, one line, no graph, in the same corner (the layer places every panel by its top right).
  Canvas small(solid_atlas(), kFpsColumns, 1, false);
  Canvas full(solid_atlas(), kColumns, 9, true);
  check(small.width() == kFpsColumns * 6 + 2 * small.graph_left() && small.width() < full.width(), "the FPS panel is narrower");
  check(!small.has_graph() && small.graph_rows() == 0 && full.has_graph(), "only the full panel has the graph");
  check(small.height() == small.text_top() + small.text_rows() + small.text_top() && small.height() < full.height() / 3,
        "the FPS panel is one line tall");
  const std::uint32_t before = pixel(small, small.width() / 2, small.height() - 2);
  small.draw_graph({16.0f, 16.0f});
  check(pixel(small, small.width() / 2, small.height() - 2) == before, "drawing a graph on it changes nothing");
}

void canvas_checks() {
  Canvas canvas(solid_atlas(), kColumns, 9, true);
  check(canvas.width() == kColumns * 6 + 2 * 5, "the panel is as wide as its text");
  check(canvas.text_rows() == 9 * 10, "the panel is as tall as its lines");
  check(canvas.graph_top() >= canvas.text_top() + canvas.text_rows(), "the graph sits under the text");
  check(canvas.graph_top() + canvas.graph_rows() < canvas.height(), "the graph fits the panel");
  check(pixel(canvas, 0, 0) == 0u, "the corners are rounded");
  check(pixel(canvas, canvas.width() - 1, canvas.height() - 1) == 0u, "all four of them");
  const std::uint32_t panel = pixel(canvas, canvas.width() / 2, 1);
  check((panel >> 24) == 0xA0u && (panel & 0xFFFFFFu) == 0u, "the panel is translucent black");

  std::vector<Line> lines(9);
  lines[0].left.push_back(Segment{"A", kRed});
  canvas.draw_text(lines);
  const std::uint32_t glyph = pixel(canvas, canvas.graph_left() + 1, canvas.text_top() + 1);
  check((glyph & 0xFFu) == (kRed & 0xFFu) && (glyph >> 24) == 0xFFu, "a solid glyph takes its colour");
  check(pixel(canvas, canvas.graph_left() + 7, canvas.text_top() + 1) == panel, "beside it, the panel");

  // A frame of 16 ms, one of 40 ms (past two 60 Hz frames) and one past the graph's top.
  canvas.draw_graph({16.0f, 40.0f, 80.0f, 16.0f});
  const int newest = canvas.graph_left() + canvas.graph_width() - 1;
  const int steady = canvas.graph_row_of(16.0);
  check((pixel(canvas, newest, steady) & 0xFFFFFFu) == (kBlue & 0xFFFFFFu), "the newest frame is on the right, in blue");
  check((pixel(canvas, newest - 1, canvas.graph_row_of(50.0)) & 0xFFFFFFu) == (kRed & 0xFFFFFFu), "a long frame is red, clamped at the top");
  check(canvas.graph_row_of(0.0) > canvas.graph_row_of(16.67) && canvas.graph_row_of(16.67) > canvas.graph_row_of(33.33),
        "a longer frame is higher");
  check(canvas.graph_row_of(1000.0) == canvas.graph_top(), "the top of the graph is its limit");
  check(pixel(canvas, canvas.graph_left(), canvas.graph_row_of(16.67)) != panel, "the guide of one 60 Hz frame");
  check(pixel(canvas, canvas.graph_left(), canvas.graph_row_of(33.33)) != panel, "the guide of two");
  canvas.draw_graph({});
  check(pixel(canvas, newest, steady) == panel || steady == canvas.graph_row_of(16.67), "the graph is cleared before it is drawn");

  // A line that is not in the panel's count is not drawn over the graph.
  std::vector<Line> too_many(11);
  too_many[10].left.push_back(Segment{"X", kRed});
  canvas.draw_graph({16.0f});
  const std::uint32_t before = pixel(canvas, canvas.graph_left() + 1, canvas.graph_top() + 1);
  canvas.draw_text(too_many);
  check(pixel(canvas, canvas.graph_left() + 1, canvas.graph_top() + 1) == before, "text never spills out of its rows");
}

void windows_checks() {
  const GlyphAtlas atlas = make_glyph_atlas(16);
  check(atlas.cell_width > 0 && atlas.cell_height >= 16, "Windows draws the font");
  check(!atlas.coverage.empty(), "the glyphs exist");
  if (atlas.coverage.empty()) return;
  int space = 0, letter = 0;
  for (int y = 0; y < atlas.cell_height; ++y) {
    for (int x = 0; x < atlas.cell_width; ++x) {
      space += atlas.at(' ', x, y);
      letter += atlas.at('M', x, y);
    }
  }
  check(space == 0 && letter > 0, "a space is empty and a letter is not");
  check(make_glyph_atlas(32).cell_height > atlas.cell_height, "a larger font has larger cells");
  // Owner, 2026-11-02: the "1" had a stroke in front of it, the edge of the "0" before it in the atlas. These glyphs have an empty
  // first column at every size the HUD uses, and each follows a glyph that reaches the edge of its cell.
  bool clean_cells = true;
  for (int size = 12; size <= 48; ++size) {
    const GlyphAtlas sized = make_glyph_atlas(size);
    for (const char glyph : {'1', '.'}) {
      for (int y = 0; y < sized.cell_height; ++y) {
        if (sized.at(glyph, 0, y) != 0) {
          std::printf("  size %d (cell %dx%d): '%c' has ink in its first column, row %d\n", size, sized.cell_width, sized.cell_height, glyph, y);
          clean_cells = false;
          break;
        }
      }
    }
  }
  check(clean_cells, "no glyph takes ink from the one before it");

  // The sampler on this process, with a thread keeping busy.
  std::atomic<int> readings{0};
  std::atomic<bool> stop{false};
  std::atomic<double> cpu{-1.0}, memory{-1.0};
  std::thread busy([&] {
    while (!stop) {}
  });
  {
    SystemSampler sampler(nullptr, [&](const SystemReading &reading) {
      cpu = reading.cpu_percent;
      memory = reading.memory_mb;
      ++readings;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (readings < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  stop = true;
  busy.join();
  const int seen = readings;
  check(seen >= 2, "the sampler reports");
  check(cpu > 0.0 && cpu <= 100.0, "a busy thread shows in the CPU line");
  check(memory > 0.0, "the memory line has a value");
  const int after = readings;
  std::this_thread::sleep_for(std::chrono::milliseconds(SystemSampler::kPeriodMs * 2));
  check(readings == after, "the sampler stops with its object");
}

}  // namespace

int main() {
  frame_time_checks();
  name_checks();
  mode_checks();
  cpu_checks();
  report_state_checks();
  text_checks();
  report_line_checks();
  fps_only_checks();
  canvas_checks();
  windows_checks();
  std::printf(failures == 0 ? "hw_hud: all checks passed\n" : "hw_hud: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
