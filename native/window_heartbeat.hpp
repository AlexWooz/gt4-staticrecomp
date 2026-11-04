#pragma once
// TASK-145: the game window's responsiveness timer. DO NOT REMOVE OR SLOW IT DOWN without reading
// docs/decisions/0076-window-responsiveness-timer.md and repeating its laptop test.
//
// Windows takes a window as "not responding" when the thread that owns it has not read a message for about 5 seconds.
// The game thread calls PeekMessage on every VBlank, but a PeekMessage that finds the queue empty does not count as a
// read (measured on the owner's laptop: IsHungAppWindow returned TRUE 0-16 ms after one). In fullscreen, with a gamepad
// and no mouse or keyboard input, the queue stays empty, so Windows declared the game window hung every few seconds.
// On NVIDIA RTX cards the shell and dwm.exe then put another window, the taskbar or the white "Ghost" window in front of
// the borderless fullscreen window for a frame and restored it with an asynchronous SetWindowPos: the desktop flashed
// and a frame came late every 6 seconds, on CoreGS 2.0 and on GSdx alike, and after Alt+Tab the game came back as a
// white "Not responding" window. A timer message every second is a real read: Windows sees the game respond, and none
// of it happens. If the game thread really stops, the timer messages wait in the queue and Windows still reports it.
#include <windows.h>

#include <cstring>

namespace gt4::window_heartbeat {

constexpr UINT_PTR timer_id = 0x145;
constexpr UINT interval_ms = 1000;
// Windows' threshold is about 5 seconds; two timer messages at least must fit in it.
static_assert(interval_ms * 2 < 5000, "the responsiveness timer must fire well within Windows' 5 second threshold");

// GT4_WINDOW_HEARTBEAT=0 turns the timer off (A/B only); unset or any other value keeps it on.
inline bool wanted(const char *switch_value) {
  return switch_value == nullptr || std::strcmp(switch_value, "0") != 0;
}

inline bool start(HWND window) {
  return SetTimer(window, timer_id, interval_ms, nullptr) != 0;
}

// The window procedure answers the timer's WM_TIMER itself: reading it from the queue is the whole job.
inline bool is_heartbeat(UINT message, WPARAM first) {
  return message == WM_TIMER && first == timer_id;
}

}  // namespace gt4::window_heartbeat
