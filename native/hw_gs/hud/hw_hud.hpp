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

#include "../../perf_report_status.hpp"

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

// What the launcher asked for (GT4_HW_HUD): "1" is the full panel, "fps" is the frame rate alone, anything else is no HUD.
enum class Mode { off, fps_only, full };
Mode mode_from_setting(const char *text);

// The performance report's line (TASK-175): none while the launcher's report is off, else what the F7/F8 keys are doing.
enum class ReportLine { hidden, idle, recording, saved };

// The report line to show now: hidden while the report is off (the launcher's Performance report), else Idle, Recording or Saved from the
// probe's status (perf_report_status.hpp). Lock free: two atomic loads.
ReportLine report_line(const perf::ReportStatus &status, std::uint64_t now_ms);

// A device name as the HUD shows it: whitespace runs (and the padding some brand strings carry) become one space, the ends are
// trimmed, and a byte outside printable ASCII becomes one '?' (the glyph atlas has no others; a UTF-8 sequence counts once).
std::string clean_device_name(const std::string &raw);

// The fit rule for a device name (the graphics card, the processor): the name is never cut. It is cleaned, then set on as many lines
// of at most `columns` characters as it needs, breaking at the spaces (a word longer than a line is split at the line's end). No
// word is dropped and none abbreviated. An empty name is no lines.
std::vector<std::string> fit_name(const std::string &raw, int columns);

// The processor's brand string from CPUID (leaves 0x80000002..4), cleaned by clean_device_name; empty when the processor reports none or
// is not x86. Read once at the start (a few instructions), never per frame. No Windows types: Linux uses <cpuid.h>.
std::string cpu_brand_string();

// What does not change while the game runs, except the output size.
struct SessionFacts {
  std::string gpu_name;
  std::string cpu_name;  // the processor's brand string; empty when not known (then there is no processor line)
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

// The HUD's text, one Line per row, at most kColumns (the full panel) or kFpsColumns (FPS only) characters wide. Full: the graphics
// card name (wrapped by fit_name), the processor name (the same), the renderer and scale with the output size and refresh rate, FPS,
// frame time, CPU, GPU, memory and VRAM, and the report line when `report` is not hidden. FPS only: one line, the frame rate.
// The number of lines depends on the names and the mode, never on the readings. The text is English only: the HUD has no language
// switch (the launcher's texts are the translated ones).
inline constexpr int kColumns = 36;
inline constexpr int kFpsColumns = 10;
std::vector<Line> hud_lines(Mode mode, ReportLine report, const SessionFacts &facts, const FrameWindow &frames, const SystemReading &system);

class Canvas {
public:
  // The height of the graph is `graph_rows` text lines. The graph's top value is kGraphTopMs.
  static constexpr double kGraphTopMs = 50.0;
  static constexpr int kGraphRows = 5;

  // `columns` characters wide and `line_count` text lines tall; `with_graph` adds the frame time graph under the text (the FPS only
  // panel has none).
  Canvas(GlyphAtlas atlas, int columns, std::size_t line_count, bool with_graph);

  int width() const { return width_; }
  int height() const { return height_; }
  // 0xAABBGGRR, premultiplied, width() * height().
  const std::vector<std::uint32_t> &pixels() const { return pixels_; }

  // The first row and the number of rows of the text and of the graph.
  int text_top() const { return padding_; }
  int text_rows() const { return static_cast<int>(line_count_) * atlas_.cell_height; }
  bool has_graph() const { return graph_height_ > 0; }
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
  std::size_t line_count_ = 0;
  int padding_ = 0, width_ = 0, height_ = 0, graph_top_ = 0, graph_height_ = 0;
  std::vector<std::uint32_t> pixels_;
};

}  // namespace gt4::hw::hud
