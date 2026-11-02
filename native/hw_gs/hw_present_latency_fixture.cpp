// CTest hw_present_latency: IN-521, the display queue control (hw_present_latency.hpp), host only.
#include "hw_present_latency.hpp"

#include <cstdio>
#include <cstdlib>

using gt4::hw::PresentLatencyControl;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "hw_present_latency: %s\n", what);
  std::exit(1);
}

constexpr double kFrame = PresentLatencyControl::kFrameMs;
constexpr int kWindow = PresentLatencyControl::kWindow;
constexpr int kSettle = PresentLatencyControl::kSettle;

void feed(PresentLatencyControl &control, int frames, double lag, double interval = kFrame) {
  for (int i = 0; i < frames; ++i) control.shown(lag, interval);
}

}  // namespace

int main() {
  {  // A short queue: nothing to do.
    PresentLatencyControl control;
    feed(control, 600, 25.0);
    check(!control.leave_out() && control.left_out() == 0, "left a frame out of a short queue");
  }
  {  // A long queue: one frame out per decision until it is under the target.
    PresentLatencyControl control;
    feed(control, kWindow, 53.0);
    check(control.leave_out(), "did not leave a frame out of a long queue");
    check(!control.leave_out(), "left out two frames for one decision");
    feed(control, kSettle + kWindow, 36.0);  // settle and judge: it helped
    feed(control, kWindow, 36.0);
    check(control.leave_out(), "36 ms is still over the target: another one out");
    feed(control, kSettle + kWindow, 20.0);
    feed(control, 600, 20.0);
    check(!control.leave_out() && control.left_out() == 2, "kept leaving frames out under the target");
    check(control.target_ms() == PresentLatencyControl::kStartTargetMs, "a helpful frame out raised the target");
  }
  {  // One low sample in a window: not a long queue, the window takes the lowest.
    PresentLatencyControl control;
    for (int i = 0; i < kWindow; ++i) control.shown(i == 10 ? 25.0 : 60.0, kFrame);
    check(!control.leave_out(), "a window with one low sample left a frame out");
  }
  {  // The display repeated a frame after one left out: as short as it goes; stay.
    PresentLatencyControl control;
    feed(control, kWindow, 38.0);
    check(control.leave_out(), "did not try at 38 ms");
    control.shown(38.0, kFrame * 2.0);
    feed(control, 600, 38.0);
    check(!control.leave_out() && control.left_out() == 1, "tried again after the display repeated a frame");
    check(control.target_ms() > 38.0, "the target did not rise over the time it settled at");
  }
  {  // No gain from a frame out (a slow display path): no endless hitches.
    PresentLatencyControl control;
    feed(control, kWindow, 60.0);
    check(control.leave_out(), "did not try at 60 ms");
    feed(control, 600, 60.0);
    check(!control.leave_out() && control.left_out() == 1, "kept trying with no gain");
  }
  {  // A queue grown by a hiccup later on is still seen after the target rose.
    PresentLatencyControl control;
    feed(control, kWindow, 38.0);
    check(control.leave_out(), "did not try at 38 ms");
    control.shown(38.0, kFrame * 2.0);
    feed(control, kSettle + kWindow - 1, 38.0);
    feed(control, kWindow, 55.0);
    check(control.leave_out(), "a grown queue was not seen after the target rose");
  }
  std::puts("hw_present_latency: ok (short queue, long queue, single sample, display repeat, no gain, grown queue)");
  return 0;
}
