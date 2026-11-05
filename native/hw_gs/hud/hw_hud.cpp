// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: see hw_hud.hpp.
#include "hw_hud.hpp"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define GT4_HUD_CPUID 1
#if defined(_MSC_VER) || defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
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
#ifdef __ANDROID__
  if (percent < 0.0) return std::string("  n/a");  // TASK-180: a reading Android does not give this app
#endif
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

#ifdef __ANDROID__
namespace {
std::string system_property(const char *name) {
  char value[PROP_VALUE_MAX] = {};
  return __system_property_get(name, value) > 0 ? std::string(value) : std::string();
}

// TASK-180: the commercial names of the common phone chips (the SoC model is all Android reports).
const char *soc_marketing_name(const std::string &model) {
  static const std::pair<const char *, const char *> kNames[] = {
      {"SM8750", "Snapdragon 8 Elite"},     {"SM8650", "Snapdragon 8 Gen 3"},   {"SM8550", "Snapdragon 8 Gen 2"},
      {"SM8475", "Snapdragon 8+ Gen 1"},    {"SM8450", "Snapdragon 8 Gen 1"},   {"SM8350", "Snapdragon 888"},
      {"SM8250", "Snapdragon 865"},         {"SM7675", "Snapdragon 7+ Gen 3"},  {"SM7550", "Snapdragon 7 Gen 3"},
  };
  for (const auto &[soc, name] : kNames)
    if (model.rfind(soc, 0) == 0) return name;
  return nullptr;
}
}  // namespace
#endif

std::string cpu_brand_string() {
#ifdef __ANDROID__
  // TASK-180: an arm64 phone has no CPUID brand string: the SoC as Android reports it (Build.SOC_MANUFACTURER and
  // Build.SOC_MODEL read these properties), with the chip's commercial name when known: "QTI SM8550 (Snapdragon 8 Gen 2)".
  const std::string model = system_property("ro.soc.model");
  std::string name = system_property("ro.soc.manufacturer");
  if (!model.empty()) name += (name.empty() ? "" : " ") + model;
  if (name.empty()) name = system_property("ro.board.platform");
  if (const char *marketing = soc_marketing_name(model)) name += std::string(" (") + marketing + ")";
  return clean_device_name(name);
#elif defined(GT4_HUD_CPUID)
  unsigned registers[4] = {};
  const auto leaf = [&registers](unsigned number) {
#if defined(_MSC_VER) || defined(_WIN32)
    int out[4];
    __cpuid(out, static_cast<int>(number));
    std::memcpy(registers, out, sizeof registers);
#else
    __cpuid(number, registers[0], registers[1], registers[2], registers[3]);
#endif
  };
  leaf(0x80000000u);
  if (registers[0] < 0x80000004u) return {};
  char brand[49] = {};
  for (unsigned part = 0; part < 3; ++part) {
    leaf(0x80000002u + part);
    std::memcpy(brand + 16 * part, registers, sizeof registers);
  }
  return clean_device_name(brand);  // the string ends at the first NUL of the 48 bytes
#else
  return {};
#endif
}

Mode mode_from_setting(const char *text) {
  if (text == nullptr) return Mode::off;
  const std::string value = text;
  if (value == "1") return Mode::full;
  if (value == "fps") return Mode::fps_only;
  return Mode::off;
}

ReportLine report_line(const perf::ReportStatus &status, std::uint64_t now_ms) {
  if (!status.enabled()) return ReportLine::hidden;
  switch (status.phase(now_ms)) {
  case perf::ReportPhase::recording: return ReportLine::recording;
  case perf::ReportPhase::saved: return ReportLine::saved;
  case perf::ReportPhase::idle: break;
  }
  return ReportLine::idle;
}

std::string clean_device_name(const std::string &raw) {
  std::string out;
  bool pending_space = false, in_sequence = false;
  for (const char character : raw) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x80 && byte < 0xC0 && in_sequence) continue;  // the rest of a UTF-8 sequence
    in_sequence = false;
    if (byte <= ' ' || byte == 0x7F) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out += ' ';
    pending_space = false;
    if (byte > '~') {
      out += '?';
      in_sequence = byte >= 0xC0;
    } else {
      out += character;
    }
  }
  return out;
}

std::vector<std::string> fit_name(const std::string &raw, int columns) {
  std::vector<std::string> lines;
  const std::string name = clean_device_name(raw);
  if (name.empty() || columns < 1) return lines;
  const std::size_t width = static_cast<std::size_t>(columns);
  std::string current;
  std::size_t start = 0;
  while (start < name.size()) {
    std::size_t end = name.find(' ', start);
    if (end == std::string::npos) end = name.size();
    std::string word = name.substr(start, end - start);
    start = end + 1;
    // A word longer than a whole line takes lines of its own; its last piece goes on like a short word.
    while (word.size() > width) {
      if (!current.empty()) {
        lines.push_back(current);
        current.clear();
      }
      lines.push_back(word.substr(0, width));
      word.erase(0, width);
    }
    if (current.empty()) {
      current = word;
    } else if (current.size() + 1 + word.size() <= width) {
      current += ' ';
      current += word;
    } else {
      lines.push_back(current);
      current = word;
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

std::vector<Line> hud_lines(Mode mode, ReportLine report, const SessionFacts &facts, const FrameWindow &frames, const SystemReading &system) {
  std::vector<Line> lines;
  if (mode == Mode::off) return lines;
  if (mode == Mode::fps_only) {
    const std::string text = frames.intervals == 0 ? std::string("  -- FPS") : formatted("%5.1f FPS", frames.fps);
    lines.push_back(Line{{Segment{text, kWhite}}, Segment{}});
    return lines;
  }

  for (const std::string &part : fit_name(facts.gpu_name, kColumns)) lines.push_back(Line{{Segment{part, kWhite}}, Segment{}});
  for (const std::string &part : fit_name(facts.cpu_name, kColumns)) lines.push_back(Line{{Segment{part, kWhite}}, Segment{}});

  char size[32];
  std::snprintf(size, sizeof size, "[%ux%u]", facts.output_width, facts.output_height);
  std::string output = size;
  if (system.refresh_hz != 0) output += " " + std::to_string(system.refresh_hz) + "Hz";
  std::string renderer = "CoreGS 2.0";
  if (!facts.scale.empty()) renderer += "  " + facts.scale + "x";
  lines.push_back(Line{{Segment{renderer, kWhite}}, Segment{output, kDim}});

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
  switch (report) {
  case ReportLine::hidden: break;
  case ReportLine::idle: lines.push_back(Line{{Segment{"Report:", kWhite}, Segment{" Idle", kDim}}, Segment{}}); break;
  case ReportLine::recording: lines.push_back(Line{{Segment{"Report:", kWhite}, Segment{" Recording", kRed}}, Segment{}}); break;
  case ReportLine::saved: lines.push_back(Line{{Segment{"Report:", kWhite}, Segment{" Saved", kGreen}}, Segment{}}); break;
  }
  return lines;
}

Canvas::Canvas(GlyphAtlas atlas, int columns, std::size_t line_count, bool with_graph) : atlas_(std::move(atlas)), line_count_(line_count) {
  padding_ = std::max(4, atlas_.cell_height / 2);
  graph_height_ = with_graph ? kGraphRows * atlas_.cell_height : 0;
  width_ = columns * atlas_.cell_width + 2 * padding_;
  graph_top_ = padding_ + text_rows() + (with_graph ? padding_ / 2 : 0);
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
    if (y + atlas_.cell_height > text_top() + text_rows()) break;  // the panel was made for this many lines
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
  if (!has_graph()) return;
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
