// CTest native_input_recording: IN-520, the pad recording file (native/input_recording.hpp), host only.
#include "input_recording.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using gt4::input_recording::format_state;
using gt4::input_recording::Player;
using gt4::input_recording::Recorder;
using gt4::pad::PadState;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "native_input_recording: %s\n", what);
  std::exit(1);
}

bool same(const PadState &a, const PadState &b) { return a.buttons == b.buttons && a.axes == b.axes && a.pressures == b.pressures; }

PadState pad_with(std::uint16_t buttons, std::uint8_t axis, std::uint8_t pressure) {
  PadState pad;
  pad.buttons = buttons;
  pad.axes[1] = axis;
  pad.pressures[3] = pressure;
  return pad;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string path = (argc > 1 ? std::string(argv[1]) : std::string("native_input_recording.txt"));
  const PadState rest;
  const PadState gas = pad_with(0x4000, 128, 255);
  const PadState steer = pad_with(0x4080, 20, 255);
  {
    Recorder recorder;
    std::string error;
    check(recorder.open(path, error), "cannot open the recording");
    recorder.vblank(1, rest);
    recorder.vblank(2, rest);
    recorder.vblank(10, gas);
    for (std::uint64_t v = 11; v < 30; ++v) recorder.vblank(v, gas);
    recorder.vblank(30, steer);
    check(recorder.finish(40) == 3, "unchanged VBlanks were written");
    check(recorder.finish(41) == 3 && !recorder.active(), "a second finish wrote again");
  }
  {
    Player player;
    std::string error;
    check(player.load(path, error), error.c_str());
    check(player.end() == 40 && player.changes() == 3, "wrong end or changes");
    check(same(player.at(1), rest) && same(player.at(9), rest), "the rest state did not hold");
    check(same(player.at(10), gas) && same(player.at(29), gas), "the gas state did not hold");
    check(same(player.at(30), steer) && same(player.at(40), steer), "the last state did not hold until the end");
    check(same(player.at(41), rest), "the pad is not at rest past the end");
  }
  {
    Player player;  // a VBlank skipped by the caller still gets every change before it
    std::string error;
    check(player.load(path, error), error.c_str());
    check(same(player.at(35), steer), "a skipped change was lost");
  }
  std::remove(path.c_str());
  const std::string header = "# GT4Recomp input recording 1\n";
  const std::string good = format_state(5, gas) + "\n";
  const auto refused = [](const std::string &text) {
    Player player;
    std::string error;
    return !player.parse(text, error) && !error.empty();
  };
  check(!refused(header + good + "end 9\n"), "a good recording was refused");
  check(refused(good + "end 9\n"), "no header was accepted");
  check(refused(header + good), "no end was accepted: an unfinished recording");
  check(refused(header + "v 5 40 8080 00\nend 9\n"), "short fields were accepted");
  check(refused(header + "v 5 zzzz 80808080 00000000000000000000000000000000\nend 9\n"), "bad hex was accepted");
  check(refused(header + format_state(9, gas) + "\n" + format_state(5, gas) + "\nend 9\n"), "VBlanks going backwards were accepted");
  check(refused(header + good + "end 9\n" + good), "text after the end was accepted");
  check(refused(header + "x 5\nend 9\n"), "an unknown line was accepted");
  std::puts("native_input_recording: ok (round trip, changes only, holds, past the end, skipped VBlanks, malformed files refused)");
  return 0;
}
