#include "input_recording.hpp"

#include <cinttypes>
#include <fstream>
#include <sstream>

namespace gt4::input_recording {
namespace {

constexpr const char *kHeader = "# GT4Recomp input recording 1";

bool same(const gt4::pad::PadState &a, const gt4::pad::PadState &b) {
  return a.buttons == b.buttons && a.axes == b.axes && a.pressures == b.pressures;
}

bool hex_value(const std::string &text, std::size_t at, std::size_t digits, std::uint32_t &value) {
  if (at + digits > text.size()) return false;
  value = 0;
  for (std::size_t i = 0; i < digits; ++i) {
    const char c = text[at + i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (digit < 0) return false;
    value = value * 16u + static_cast<std::uint32_t>(digit);
  }
  return true;
}

bool parse_bytes(const std::string &text, std::uint8_t *out, std::size_t count) {
  if (text.size() != count * 2u) return false;
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t value = 0;
    if (!hex_value(text, i * 2u, 2u, value)) return false;
    out[i] = static_cast<std::uint8_t>(value);
  }
  return true;
}

}  // namespace

std::string format_state(std::uint64_t vblank, const gt4::pad::PadState &pad) {
  char line[128];
  int used = std::snprintf(line, sizeof line, "v %" PRIu64 " %04x ", vblank, pad.buttons);
  for (const std::uint8_t axis : pad.axes) used += std::snprintf(line + used, sizeof line - static_cast<std::size_t>(used), "%02x", axis);
  line[used++] = ' ';
  for (const std::uint8_t pressure : pad.pressures) {
    used += std::snprintf(line + used, sizeof line - static_cast<std::size_t>(used), "%02x", pressure);
  }
  return std::string(line, static_cast<std::size_t>(used));
}

Recorder::~Recorder() {
  if (file_) std::fclose(file_);
}

bool Recorder::open(const std::string &path, std::string &error) {
  file_ = std::fopen(path.c_str(), "w");
  if (!file_) {
    error = "cannot write " + path;
    return false;
  }
  std::fprintf(file_, "%s\n", kHeader);
  std::fflush(file_);
  return true;
}

void Recorder::vblank(std::uint64_t vblank, const gt4::pad::PadState &pad) {
  if (!file_ || (any_ && same(pad, last_))) return;
  any_ = true;
  last_ = pad;
  ++changes_;
  std::fprintf(file_, "%s\n", format_state(vblank, pad).c_str());
  std::fflush(file_);
}

std::uint64_t Recorder::finish(std::uint64_t last_vblank) {
  if (file_) {
    std::fprintf(file_, "end %" PRIu64 "\n", last_vblank);
    std::fclose(file_);
    file_ = nullptr;
  }
  return changes_;
}

bool Player::load(const std::string &path, std::string &error) {
  std::ifstream file(path);
  if (!file) {
    error = "cannot read " + path;
    return false;
  }
  std::ostringstream text;
  text << file.rdbuf();
  return parse(text.str(), error);
}

bool Player::parse(const std::string &text, std::string &error) {
  changes_.clear();
  next_ = 0;
  end_ = 0;
  current_ = rest_;
  std::istringstream lines(text);
  std::string line;
  std::size_t number = 0;
  bool header = false;
  while (std::getline(lines, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto bad = [&](const char *what) {
      error = "line " + std::to_string(number) + ": " + what;
      return false;
    };
    if (line == kHeader) {
      header = true;
      continue;
    }
    if (line.empty()) continue;
    if (!header) return bad("no recording header");
    if (end_ != 0) return bad("text after the end line");
    std::istringstream fields(line);
    std::string kind;
    std::uint64_t vblank = 0;
    if (!(fields >> kind >> vblank) || vblank == 0) return bad("no VBlank");
    if (!changes_.empty() && vblank < changes_.back().vblank) return bad("VBlanks go backwards");
    if (kind == "end") {
      end_ = vblank;
      continue;
    }
    if (kind != "v") return bad("unknown line");
    std::string buttons, axes, pressures;
    if (!(fields >> buttons >> axes >> pressures)) return bad("missing fields");
    Change change;
    change.vblank = vblank;
    std::uint32_t value = 0;
    if (buttons.size() != 4u || !hex_value(buttons, 0, 4, value)) return bad("bad buttons");
    change.pad.buttons = static_cast<std::uint16_t>(value);
    if (!parse_bytes(axes, change.pad.axes.data(), change.pad.axes.size())) return bad("bad axes");
    if (!parse_bytes(pressures, change.pad.pressures.data(), change.pad.pressures.size())) return bad("bad pressures");
    changes_.push_back(change);
  }
  if (!header) {
    error = "no recording header";
    return false;
  }
  if (end_ == 0) {
    error = "no end line: the recording was not finished with F9";
    return false;
  }
  return true;
}

const gt4::pad::PadState &Player::at(std::uint64_t vblank) {
  if (vblank > end_) return rest_;
  while (next_ < changes_.size() && changes_[next_].vblank <= vblank) {
    current_ = changes_[next_].pad;
    ++next_;
  }
  return current_;
}

}  // namespace gt4::input_recording
