// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: see hw_hud.hpp.
#include "hw_hud.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace gt4::hw::hud {
namespace {

constexpr std::uint32_t kPanel = 0xA0000000u;  // black, 63% opaque
constexpr std::uint32_t kGuide = 0x50FFFFFFu;
constexpr std::uint32_t kGraphLine = kBlue;
constexpr std::uint32_t kGraphSpike = kRed;
constexpr double kOneFrameMs = 1000.0 / 60.0;

// Premultiplies a straight colour.
std::uint32_t premultiplied(std::uint32_t colour, std::uint32_t coverage) {
  const std::uint32_t alpha = ((colour >> 24) * coverage + 127u) / 255u;
  const std::uint32_t red = ((colour & 0xFFu) * alpha + 127u) / 255u;
  const std::uint32_t green = (((colour >> 8) & 0xFFu) * alpha + 127u) / 255u;
  const std::uint32_t blue = (((colour >> 16) & 0xFFu) * alpha + 127u) / 255u;
  return (alpha << 24) | (blue << 16) | (green << 8) | red;
}

std::string formatted(const char *pattern, double a, double b = 0.0, double c = 0.0) {
  char text[64];
  std::snprintf(text, sizeof text, pattern, a, b, c);
  return text;
}

std::string percent_text(double percent) {
  return percent < 0.0 ? std::string("   --") : formatted("%5.1f%%", percent);
}

std::string memory_text(double mb) {
  if (mb < 0.0) return "--";
  return mb >= 1024.0 ? formatted("%.2f GB", mb / 1024.0) : formatted("%.0f MB", mb);
}

}  // namespace

std::uint8_t GlyphAtlas::at(char glyph, int x, int y) const {
  int index = static_cast<unsigned char>(glyph);
  if (index < kFirst || index > kLast) index = '?';
  const std::size_t cell = static_cast<std::size_t>(index - kFirst) * static_cast<std::size_t>(cell_width);
  return coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(cell_width) * (kLast - kFirst + 1) + cell + static_cast<std::size_t>(x)];
}

FrameTimes::FrameTimes() : times_(kCapacity, 0.0) {}

void FrameTimes::note_present(double now_ms) {
  times_[next_] = now_ms;
  next_ = (next_ + 1) % kCapacity;
  stored_ = std::min(stored_ + 1, kCapacity);
}

FrameWindow FrameTimes::window(double now_ms) const {
  FrameWindow out;
  double newest = 0.0, oldest = 0.0, low = 0.0, high = 0.0;
  for (std::size_t back = 1; back <= stored_; ++back) {
    const double time = times_[(next_ + kCapacity - back) % kCapacity];
    if (now_ms - time > kWindowMs) break;
    if (back == 1) {
      newest = time;
    } else {
      const double interval = oldest - time;
      low = out.intervals == 0 ? interval : std::min(low, interval);
      high = out.intervals == 0 ? interval : std::max(high, interval);
      ++out.intervals;
    }
    oldest = time;
  }
  if (out.intervals == 0 || newest <= oldest) return FrameWindow{};
  out.frame_ms = (newest - oldest) / out.intervals;
  out.frame_ms_low = low;
  out.frame_ms_high = high;
  out.fps = 1000.0 / out.frame_ms;
  out.fps_low = high > 0.0 ? 1000.0 / high : 0.0;
  out.fps_high = low > 0.0 ? 1000.0 / low : 0.0;
  return out;
}

void FrameTimes::recent_intervals(std::vector<float> &out, std::size_t count) const {
  out.clear();
  if (stored_ < 2) return;
  const std::size_t have = std::min(count, stored_ - 1);
  for (std::size_t back = have; back >= 1; --back) {
    const double newer = times_[(next_ + kCapacity - back) % kCapacity];
    const double older = times_[(next_ + kCapacity - back - 1) % kCapacity];
    out.push_back(static_cast<float>(newer - older));
  }
}

std::vector<Line> hud_lines(const SessionFacts &facts, const FrameWindow &frames, const SystemReading &system) {
  std::vector<Line> lines;
  lines.reserve(kLineCount);

  char size[32];
  std::snprintf(size, sizeof size, "[%ux%u]", facts.output_width, facts.output_height);
  const std::size_t room = static_cast<std::size_t>(kColumns) - std::string(size).size() - 1;
  std::string name = facts.gpu_name;
  if (name.size() > room) name = name.substr(0, room - 2) + "..";
  lines.push_back(Line{{Segment{name, kWhite}}, Segment{size, kDim}});

  std::string renderer = "CoreGS 2.0";
  if (!facts.scale.empty()) renderer += "  " + facts.scale + "x";
  std::string refresh;
  if (system.refresh_hz != 0) refresh = std::to_string(system.refresh_hz) + "Hz";
  lines.push_back(Line{{Segment{renderer, kWhite}}, Segment{refresh, kDim}});

  if (frames.intervals == 0) {
    lines.push_back(Line{{Segment{"FPS:      --", kWhite}}, Segment{}});
    lines.push_back(Line{{Segment{"Frame:    --", kBlue}}, Segment{}});
  } else {
    lines.push_back(Line{{Segment{"FPS:   " + formatted("%6.2f", frames.fps) + " [", kWhite},
                          Segment{formatted("%6.2f", frames.fps_low), kRed},
                          Segment{formatted(" %6.2f", frames.fps_high) + "]", kWhite}},
                         Segment{}});
    lines.push_back(Line{{Segment{"Frame: " + formatted("%6.2f [%6.2f", frames.frame_ms, frames.frame_ms_low), kBlue},
                          Segment{formatted(" %6.2f", frames.frame_ms_high), kRed},
                          Segment{"] ms", kBlue}},
                         Segment{}});
  }
  lines.push_back(Line{{Segment{"CPU:   " + percent_text(system.cpu_percent), kGreen}}, Segment{}});
  lines.push_back(Line{{Segment{"GPU:   " + percent_text(system.gpu_percent), kGreen}}, Segment{}});
  lines.push_back(Line{{Segment{"Mem:   " + memory_text(system.memory_mb), kWhite}},
                       Segment{"VRAM: " + memory_text(system.video_memory_mb), kWhite}});
  return lines;
}

Canvas::Canvas(GlyphAtlas atlas) : atlas_(std::move(atlas)) {
  padding_ = std::max(4, atlas_.cell_height / 2);
  graph_height_ = kGraphRows * atlas_.cell_height;
  width_ = kColumns * atlas_.cell_width + 2 * padding_;
  graph_top_ = padding_ + text_rows() + padding_ / 2;
  height_ = graph_top_ + graph_height_ + padding_;
  pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), premultiplied(kPanel, 255u));
  // The panel's rounded corners.
  const int radius = padding_;
  for (int y = 0; y < radius; ++y) {
    for (int x = 0; x < radius; ++x) {
      const double dx = radius - x - 0.5, dy = radius - y - 0.5;
      if (dx * dx + dy * dy <= static_cast<double>(radius) * radius) continue;
      for (const int row : {y, height_ - 1 - y}) {
        pixels_[static_cast<std::size_t>(row) * width_ + x] = 0u;
        pixels_[static_cast<std::size_t>(row) * width_ + (width_ - 1 - x)] = 0u;
      }
    }
  }
}

void Canvas::clear_rows(int top, int rows) {
  const std::uint32_t panel = premultiplied(kPanel, 255u);
  for (int y = top; y < top + rows; ++y) {
    std::uint32_t *row = pixels_.data() + static_cast<std::size_t>(y) * width_;
    std::fill(row + padding_, row + width_ - padding_, panel);
  }
}

void Canvas::blend(int x, int y, std::uint32_t colour, std::uint32_t coverage) {
  if (coverage == 0u || x < 0 || y < 0 || x >= width_ || y >= height_) return;
  std::uint32_t &pixel = pixels_[static_cast<std::size_t>(y) * width_ + x];
  const std::uint32_t source = premultiplied(colour, coverage);
  const std::uint32_t keep = 255u - (source >> 24);
  std::uint32_t out = 0u;
  for (int shift = 0; shift < 32; shift += 8) {
    const std::uint32_t mixed = ((source >> shift) & 0xFFu) + (((pixel >> shift) & 0xFFu) * keep + 127u) / 255u;
    out |= std::min(mixed, 255u) << shift;
  }
  pixel = out;
}

void Canvas::draw_string(int x, int y, const std::string &text, std::uint32_t colour) {
  for (const char glyph : text) {
    for (int row = 0; row < atlas_.cell_height; ++row) {
      for (int column = 0; column < atlas_.cell_width; ++column) {
        blend(x + column, y + row, colour, atlas_.at(glyph, column, row));
      }
    }
    x += atlas_.cell_width;
  }
}

void Canvas::draw_text(const std::vector<Line> &lines) {
  clear_rows(text_top(), text_rows());
  int y = text_top();
  for (const Line &line : lines) {
    int x = padding_;
    for (const Segment &segment : line.left) {
      draw_string(x, y, segment.text, segment.colour);
      x += static_cast<int>(segment.text.size()) * atlas_.cell_width;
    }
    const int right = width_ - padding_ - static_cast<int>(line.right.text.size()) * atlas_.cell_width;
    draw_string(right, y, line.right.text, line.right.colour);
    y += atlas_.cell_height;
  }
}

int Canvas::graph_row_of(double ms) const {
  const double shown = std::clamp(ms, 0.0, kGraphTopMs);
  const int row = static_cast<int>((1.0 - shown / kGraphTopMs) * (graph_height_ - 1) + 0.5);
  return graph_top_ + row;
}

void Canvas::draw_graph(const std::vector<float> &intervals_ms) {
  clear_rows(graph_top_, graph_height_);
  const int left = graph_left(), columns = graph_width();
  for (const double guide : {kOneFrameMs, 2.0 * kOneFrameMs}) {
    const int row = graph_row_of(guide);
    for (int x = left; x < left + columns; ++x) blend(x, row, kGuide, 255u);
  }
  const int floor_row = graph_row_of(0.0);
  for (int x = left; x < left + columns; ++x) blend(x, floor_row, kGuide, 255u);

  const int count = std::min(static_cast<int>(intervals_ms.size()), columns);
  const std::size_t first = intervals_ms.size() - static_cast<std::size_t>(count);
  const int thickness = std::max(1, atlas_.cell_height / 12);
  int previous_row = -1;
  for (int i = 0; i < count; ++i) {
    const double ms = intervals_ms[first + static_cast<std::size_t>(i)];
    const int x = left + columns - count + i;
    const int row = graph_row_of(ms);
    const int from = previous_row < 0 ? row : std::min(previous_row, row);
    const int to = previous_row < 0 ? row : std::max(previous_row, row);
    const std::uint32_t colour = ms > 2.0 * kOneFrameMs ? kGraphSpike : kGraphLine;
    for (int y = from; y <= to + thickness - 1; ++y) {
      if (y < graph_top_ + graph_height_) blend(x, y, colour, 255u);
    }
    previous_row = row;
  }
}

}  // namespace gt4::hw::hud
