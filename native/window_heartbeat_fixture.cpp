// TASK-145: the game window's responsiveness timer (window_heartbeat.hpp). A hidden window with the timer started must
// find the timer's WM_TIMER in its queue through the same PeekMessage loop as the game, well within Windows' 5 second
// "not responding" threshold; the A/B switch must keep the timer on unless it is exactly "0".
#include "window_heartbeat.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "window_heartbeat_fixture: FAILED: %s\n", what);
    std::exit(1);
  }
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM first, LPARAM second) {
  if (gt4::window_heartbeat::is_heartbeat(message, first)) return 0;
  return DefWindowProcW(window, message, first, second);
}

}  // namespace

int main() {
  require(gt4::window_heartbeat::wanted(nullptr), "on when GT4_WINDOW_HEARTBEAT is unset");
  require(gt4::window_heartbeat::wanted("1"), "on with GT4_WINDOW_HEARTBEAT=1");
  require(gt4::window_heartbeat::wanted(""), "on with an empty GT4_WINDOW_HEARTBEAT");
  require(!gt4::window_heartbeat::wanted("0"), "off with GT4_WINDOW_HEARTBEAT=0");
  require(gt4::window_heartbeat::is_heartbeat(WM_TIMER, gt4::window_heartbeat::timer_id), "the timer's WM_TIMER");
  require(!gt4::window_heartbeat::is_heartbeat(WM_TIMER, gt4::window_heartbeat::timer_id + 1), "another timer");
  require(!gt4::window_heartbeat::is_heartbeat(WM_PAINT, gt4::window_heartbeat::timer_id), "another message");

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = procedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"GT4WindowHeartbeatFixture";
  require(RegisterClassW(&window_class) != 0, "window class registered");
  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"heartbeat fixture", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, window_class.hInstance, nullptr);
  require(window != nullptr, "hidden window created");
  require(gt4::window_heartbeat::start(window), "timer started");

  // The game's loop: PeekMessage with no filter, then other work (here a short sleep).
  const ULONGLONG start = GetTickCount64();
  const ULONGLONG limit = 3u * gt4::window_heartbeat::interval_ms + 500u;
  int heartbeats = 0;
  ULONGLONG first_at = 0;
  while (heartbeats < 2 && GetTickCount64() - start < limit) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.hwnd == window && gt4::window_heartbeat::is_heartbeat(message.message, message.wParam)) {
        if (heartbeats++ == 0) first_at = GetTickCount64() - start;
      }
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  const ULONGLONG elapsed = GetTickCount64() - start;
  DestroyWindow(window);
  std::printf("window_heartbeat_fixture: %d timer messages in %llu ms (first after %llu ms)\n", heartbeats, elapsed,
              first_at);
  require(heartbeats >= 2, "two timer messages read through the PeekMessage loop");
  require(first_at < 2500, "the first timer message well within Windows' 5 second threshold");
  std::puts("window_heartbeat_fixture: OK");
  return 0;
}
