// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: the performance HUD on known inputs: the numbers, the text, the panel's rows, and what Windows gives it.
#include "hw_hud.hpp"
#include "hw_hud_system.hpp"

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

void text_checks() {
  SessionFacts facts;
  facts.gpu_name = "AMD Radeon RX 9060 XT";
  facts.scale = "3";
  facts.output_width = 3440;
  facts.output_height = 1440;
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
  const std::vector<Line> lines = hud_lines(facts, frames, reading);
  check(lines.size() == kLineCount, "seven lines");
  check(text_of(lines[0]) == "AMD Radeon RX 9060 XT" && lines[0].right.text == "[3440x1440]", "the card and the output");
  check(text_of(lines[1]) == "CoreGS 2.0  3x" && lines[1].right.text == "144Hz", "the renderer, the scale and the refresh rate");
  check(text_of(lines[2]) == "FPS:    59.94 [ 58.20  60.50]", "the FPS line");
  check(text_of(lines[3]) == "Frame:  16.68 [ 16.53  17.18] ms", "the frame time line");
  check(text_of(lines[4]) == "CPU:    31.2%", "the CPU line says CPU, not EE");
  check(text_of(lines[5]) == "GPU:    46.0%", "the GPU line says GPU, not GS");
  check(text_of(lines[6]) == "Mem:   812 MB" && lines[6].right.text == "VRAM: 2.10 GB", "the memory line");
  for (const Line &line : lines) {
    check(static_cast<int>(text_of(line).size() + line.right.text.size()) <= kColumns, "the line fits the panel");
  }

  facts.gpu_name = "A graphics card with a very long marketing name indeed";
  const std::vector<Line> unknown = hud_lines(facts, FrameWindow{}, SystemReading{});
  check(static_cast<int>(text_of(unknown[0]).size() + 1 + unknown[0].right.text.size()) <= kColumns, "a long card name is cut");
  check(text_of(unknown[0]).find("..") != std::string::npos, "a cut name says so");
  check(text_of(unknown[2]) == "FPS:      --" && text_of(unknown[4]) == "CPU:      --" && text_of(unknown[6]) == "Mem:   --",
        "what is not known yet shows as dashes");
  check(unknown[1].right.text.empty(), "no refresh rate until it is known");
}

void canvas_checks() {
  Canvas canvas(solid_atlas());
  check(canvas.width() == kColumns * 6 + 2 * 5, "the panel is as wide as its text");
  check(canvas.graph_top() >= canvas.text_top() + canvas.text_rows(), "the graph sits under the text");
  check(canvas.graph_top() + canvas.graph_rows() < canvas.height(), "the graph fits the panel");
  check(pixel(canvas, 0, 0) == 0u, "the corners are rounded");
  check(pixel(canvas, canvas.width() - 1, canvas.height() - 1) == 0u, "all four of them");
  const std::uint32_t panel = pixel(canvas, canvas.width() / 2, 1);
  check((panel >> 24) == 0xA0u && (panel & 0xFFFFFFu) == 0u, "the panel is translucent black");

  std::vector<Line> lines(kLineCount);
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
  text_checks();
  canvas_checks();
  windows_checks();
  std::printf(failures == 0 ? "hw_hud: all checks passed\n" : "hw_hud: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
