#pragma once
// IN-520: the pad state the game receives, recorded per guest VBlank and replayed at the same VBlanks, so a bench repeats a
// real drive however fast it runs. Text file, one line per change, so it can be read and compared:
//   # GT4Recomp input recording 1
//   v <vblank> <buttons, 4 hex> <4 axes, 2 hex each> <16 pressures, 2 hex each>
//   end <vblank>
// A state holds until the next line. The first VBlank is 1. Past `end` the pad is at rest.

#include "pad_mapping.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gt4::input_recording {

std::string format_state(std::uint64_t vblank, const gt4::pad::PadState &pad);

// Writes each change at once, so a crash keeps the recording up to its last change.
class Recorder {
public:
  Recorder() = default;
  Recorder(const Recorder &) = delete;
  Recorder &operator=(const Recorder &) = delete;
  ~Recorder();
  bool open(const std::string &path, std::string &error);
  void vblank(std::uint64_t vblank, const gt4::pad::PadState &pad);
  // Writes `end`; later calls do nothing. Returns the number of changes written.
  std::uint64_t finish(std::uint64_t last_vblank);
  bool active() const { return file_ != nullptr; }

private:
  std::FILE *file_ = nullptr;
  bool any_ = false;
  gt4::pad::PadState last_{};
  std::uint64_t changes_ = 0;
};

class Player {
public:
  struct Change {
    std::uint64_t vblank = 0;
    gt4::pad::PadState pad;
  };
  // False with `error` for a missing file or any malformed line: a bench never replays half a drive.
  bool load(const std::string &path, std::string &error);
  bool parse(const std::string &text, std::string &error);
  // The state at this VBlank; VBlanks must not go backwards.
  const gt4::pad::PadState &at(std::uint64_t vblank);
  std::uint64_t end() const { return end_; }
  std::size_t changes() const { return changes_.size(); }

private:
  std::vector<Change> changes_;
  std::size_t next_ = 0;
  std::uint64_t end_ = 0;
  gt4::pad::PadState current_{};
  gt4::pad::PadState rest_{};
};

}  // namespace gt4::input_recording
