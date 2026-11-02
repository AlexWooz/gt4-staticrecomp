// CTest hw_present_latency: IN-521, the display queue and phase control (hw_present_latency.hpp), host only.
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

// A display at the game's rate: `phaseMs` from presentation to the next flip, plus `pathMs` and the queue.
struct Display {
  double pathMs = 25.0, phaseMs = 14.0;
  int queue = 0;
  double lag() const { return pathMs + phaseMs + queue * kFrame; }
  void shift_later(double ms) {
    phaseMs -= ms;
    while (phaseMs < 0.0) phaseMs += kFrame;  // missed its flip: a whole refresh to wait for the next
    while (phaseMs >= kFrame) phaseMs -= kFrame;  // earlier than a flip it had missed: that flip takes it again
  }
};

}  // namespace

int main() {
  {  // Under the target, queue left alone; no walking at all when told not to.
    PresentLatencyControl control(false);
    feed(control, 600, 25.0);
    check(!control.leave_out() && control.left_out() == 0, "left a frame out of a short queue");
    check(control.take_grid_shift_us() == 0, "walked with the walk off");
  }
  {  // A long queue: one frame out per decision until it is under the target.
    PresentLatencyControl control(false);
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
    PresentLatencyControl control(false);
    for (int i = 0; i < kWindow; ++i) control.shown(i == 10 ? 25.0 : 60.0, kFrame);
    check(!control.leave_out(), "a window with one low sample left a frame out");
  }
  {  // The display repeated a frame after one left out: as short as it goes; stay.
    PresentLatencyControl control(false);
    feed(control, kWindow, 38.0);
    check(control.leave_out(), "did not try at 38 ms");
    control.shown(38.0, kFrame * 2.0);
    feed(control, 600, 38.0);
    check(!control.leave_out() && control.left_out() == 1, "tried again after the display repeated a frame");
    check(control.target_ms() > 38.0, "the target did not rise over the time it settled at");
  }
  {  // No gain from a frame out (a slow display path): no endless hitches.
    PresentLatencyControl control(false);
    feed(control, kWindow, 60.0);
    check(control.leave_out(), "did not try at 60 ms");
    feed(control, 600, 60.0);
    check(!control.leave_out() && control.left_out() == 1, "kept trying with no gain");
  }
  {  // The walk: later in steps until the time reaches its goal, with no miss, then still.
    PresentLatencyControl control(true, 45.0);
    Display display;
    display.phaseMs = 14.0;  // 39 ms
    int steps = 0;
    for (int window = 0; window < 60; ++window) {
      feed(control, kWindow, display.lag());
      const double shift = control.take_grid_shift_us() / 1000.0;
      check(shift >= 0.0, "stepped back with no miss");
      if (shift > 0.0) ++steps;
      display.shift_later(shift);
      check(!control.leave_out(), "left a frame out under the target");
    }
    check(steps >= 6, "the phase did not walk");
    check(display.lag() <= PresentLatencyControl::kWalkGoalMs, "the time did not reach the goal");
    check(control.walks() == 1, "the phase walked again with the time at the goal");
  }
  {  // A slower display path: a miss before the goal; back by the margin, and no walk for that time again.
    PresentLatencyControl control(true, 70.0);
    Display display;
    display.pathMs = 35.0;
    display.phaseMs = 3.0;  // 38 ms
    bool stepped_back = false;
    for (int window = 0; window < 200; ++window) {
      feed(control, kWindow, display.lag());
      const double shift = control.take_grid_shift_us() / 1000.0;
      if (shift < 0.0) stepped_back = true;
      display.shift_later(shift);
    }
    check(stepped_back, "the phase did not step back after the miss");
    check(display.lag() - display.pathMs <= PresentLatencyControl::kMarginMs + PresentLatencyControl::kWalkStepMs + 0.5,
          "the frame did not settle within the margin of its flip");
    check(control.walks() == 1, "the phase walked again after it found the flip");
  }
  {  // A hitch that looks like a miss at a high time: another walk later, and a limit on them.
    PresentLatencyControl control(true, 200.0);
    double lag = 38.0;
    int backs = 0;
    for (int window = 0; window < 2000; ++window) {
      feed(control, kWindow, window % 40 == 39 ? lag + 30.0 : lag);
      if (control.take_grid_shift_us() < 0) ++backs;
      lag = 38.0;
    }
    check(control.walks() >= 2, "no other walk after a hitch");
    check(control.walks() <= PresentLatencyControl::kMaxWalks, "too many walks");
  }
  {  // A faster display alternates intervals: no walk.
    PresentLatencyControl control;
    for (int i = 0; i < 600; ++i) control.shown(20.0, i % 5 < 3 ? 13.9 : 20.8);
    check(control.take_grid_shift_us() == 0 && control.walks() == 0, "walked on a display faster than the game");
  }
  {  // Already close to the flip: no walk.
    PresentLatencyControl control;
    feed(control, 600, 8.0);
    check(control.take_grid_shift_us() == 0, "walked with the frame close to its flip");
  }
  std::puts("hw_present_latency: ok (short queue, long queue, single sample, display repeat, no gain, walk, fast display, close to the flip)");
  return 0;
}
