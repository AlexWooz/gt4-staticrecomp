// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
//
// TASK-135: the performance HUD of CoreGS 2.0 (GT4_HW_HUD=1, off by default), what it shows and how it is drawn on the CPU. It reads
// the host only and is drawn over the finished picture at present time, so the game never sees it.
//
// No Vulkan and no Windows types here (hud/hw_hud_system.* reads the machine, hud/hw_hud_layer.* puts the result on screen):
//   FrameTimes  when pictures were presented: FPS and frame time over the last second, and the graph's history.
//   hud_lines   the text, one line per metric, each with its colour.
//   Canvas      the panel as RGBA pixels (premultiplied alpha): the text and the live frame time graph.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::hw::hud {

// Colours are 0xAABBGGRR, the byte order of R8G8B8A8.
inline constexpr std::uint32_t kWhite = 0xFFFFFFFFu;
inline constexpr std::uint32_t kRed = 0xFF8A8AFFu;
inline constexpr std::uint32_t kBlue = 0xFFFFB98Au;
inline constexpr std::uint32_t kGreen = 0xFF9DE89Du;
inline constexpr std::uint32_t kDim = 0xFFB0B0B0u;

// The printable ASCII glyphs, from ' ' to '~', in one row of monospaced cells. A byte is how much of the pixel the glyph covers.
struct GlyphAtlas {
  static constexpr int kFirst = 32, kLast = 126;
  int cell_width = 0, cell_height = 0;
  std::vector<std::uint8_t> coverage;  // cell_width * cell_height * glyph count

  std::uint8_t at(char glyph, int x, int y) const;
};

// What the machine reports, about twice a second. A negative value is not known (the line then shows "--").
struct SystemReading {
  double cpu_percent = -1.0;  // the game's share of all the processor's threads, as in Task Manager
  double gpu_percent = -1.0;  // the game's busiest graphics card engine, as in Task Manager
  double memory_mb = -1.0;
  double video_memory_mb = -1.0;
  std::uint32_t refresh_hz = 0;
};

// What does not change while the game runs, except the output size.
struct SessionFacts {
  std::string gpu_name;
  std::string scale;  // the render scale's multiplier as text ("3"); empty when not known
  std::uint32_t output_width = 0, output_height = 0;
};

struct FrameWindow {
  std::uint32_t intervals = 0;  // 0: nothing to show yet
  double fps = 0.0, fps_low = 0.0, fps_high = 0.0;
  double frame_ms = 0.0, frame_ms_low = 0.0, frame_ms_high = 0.0;
};

// The times of the last presentations, oldest first once read. Not thread safe: Layer holds the lock.
class FrameTimes {
public:
  static constexpr std::size_t kCapacity = 1024;
  static constexpr double kWindowMs = 1000.0;

  FrameTimes();
  void note_present(double now_ms);
  // The presentations of the last second before `now_ms`.
  FrameWindow window(double now_ms) const;
  // The intervals between the last presentations, in ms, oldest first: at most `count` of them.
  void recent_intervals(std::vector<float> &out, std::size_t count) const;

private:
  std::vector<double> times_;
  std::size_t next_ = 0, stored_ = 0;
};

struct Segment {
  std::string text;
  std::uint32_t colour = kWhite;
};

struct Line {
  std::vector<Segment> left;
  Segment right;  // drawn against the right edge
};

// The HUD's text. Fits `columns` characters per line; a long graphics card name is cut.
inline constexpr int kColumns = 34;
inline constexpr std::size_t kLineCount = 7;
std::vector<Line> hud_lines(const SessionFacts &facts, const FrameWindow &frames, const SystemReading &system);

class Canvas {
public:
  // The height of the graph is `graph_rows` text lines. The graph's top value is kGraphTopMs.
  static constexpr double kGraphTopMs = 50.0;
  static constexpr int kGraphRows = 5;

  explicit Canvas(GlyphAtlas atlas);

  int width() const { return width_; }
  int height() const { return height_; }
  // 0xAABBGGRR, premultiplied, width() * height().
  const std::vector<std::uint32_t> &pixels() const { return pixels_; }

  // The first row and the number of rows of the text and of the graph.
  int text_top() const { return padding_; }
  int text_rows() const { return static_cast<int>(kLineCount) * atlas_.cell_height; }
  int graph_top() const { return graph_top_; }
  int graph_rows() const { return graph_height_; }
  int graph_left() const { return padding_; }
  int graph_width() const { return width_ - 2 * padding_; }

  void draw_text(const std::vector<Line> &lines);
  // One column per interval, newest on the right; the guides are one and two 60 Hz frames.
  void draw_graph(const std::vector<float> &intervals_ms);
  // The row of the graph where a frame time is drawn (top of the graph: kGraphTopMs).
  int graph_row_of(double ms) const;

private:
  void clear_rows(int top, int rows);
  void draw_string(int x, int y, const std::string &text, std::uint32_t colour);
  void blend(int x, int y, std::uint32_t colour, std::uint32_t coverage);

  GlyphAtlas atlas_;
  int padding_ = 0, width_ = 0, height_ = 0, graph_top_ = 0, graph_height_ = 0;
  std::vector<std::uint32_t> pixels_;
};

}  // namespace gt4::hw::hud
