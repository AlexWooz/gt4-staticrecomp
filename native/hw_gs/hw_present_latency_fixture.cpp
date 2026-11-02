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

constexpr int kWindow = PresentLatencyControl::kWindow;
constexpr int kSettle = PresentLatencyControl::kSettle;

void feed(PresentLatencyControl &control, int frames, double lag) {
  for (int i = 0; i < frames; ++i) control.shown(lag);
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
    for (int i = 0; i < kWindow; ++i) control.shown(i == 10 ? 25.0 : 60.0);
    check(!control.leave_out(), "a window with one low sample left a frame out");
  }
  {  // The shortest queue this display takes: a frame out does not help; stay there.
    PresentLatencyControl control;
    feed(control, kWindow, 38.0);
    check(control.leave_out(), "did not try at 38 ms");
    feed(control, 600, 38.0);
    check(!control.leave_out() && control.left_out() == 1, "tried again at the shortest queue");
    check(control.target_ms() > 38.0, "the target did not rise over the time it settled at");
  }
  {  // A queue grown by a skipped refresh later on is still seen after the target rose, and goes back down.
    PresentLatencyControl control;
    feed(control, kWindow, 38.0);
    check(control.leave_out(), "did not try at 38 ms");
    feed(control, kSettle + kWindow, 38.0);
    feed(control, kWindow, 55.0);
    check(control.leave_out(), "a grown queue was not seen after the target rose");
    feed(control, kSettle + kWindow, 38.0);
    feed(control, 600, 38.0);
    check(!control.leave_out() && control.left_out() == 2, "kept trying back at the shortest queue");
  }
  {  // The display skips a refresh just as a frame goes out: no gain at a level clearly over the lowest seen. Not the shortest
     // queue: a second try reaches it, and the target did not rise.
    PresentLatencyControl control;
    feed(control, kWindow, 31.0);
    feed(control, kWindow, 48.0);
    check(control.leave_out(), "did not try at 48 ms");
    feed(control, kSettle + kWindow, 48.0);
    check(control.leave_out(), "no second try over the lowest seen");
    feed(control, kSettle + kWindow, 31.0);
    feed(control, 600, 31.0);
    check(!control.leave_out() && control.left_out() == 2, "kept trying after the second try helped");
    check(control.target_ms() == PresentLatencyControl::kStartTargetMs, "the target rose with a queue that could shorten");
  }
  {  // A lower time in the menus does not make a race's shortest queue an endless series of hitches: two tries, then it stays.
    PresentLatencyControl control;
    feed(control, kWindow, 17.0);
    feed(control, kWindow, 37.0);
    check(control.leave_out(), "did not try at 37 ms");
    feed(control, kSettle + kWindow, 37.0);
    check(control.leave_out(), "no second try over the lowest seen");
    feed(control, kSettle + kWindow, 37.0);
    feed(control, 600, 37.0);
    check(!control.leave_out() && control.left_out() == 2, "kept trying at the shortest queue");
    check(control.target_ms() > 37.0, "the target did not rise over the time it settled at");
  }
  std::puts("hw_present_latency: ok (short queue, long queue, single sample, shortest queue, grown queue, skipped refresh, menus)");
  return 0;
}
