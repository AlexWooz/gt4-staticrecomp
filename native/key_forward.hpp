#pragma once

// TASK-154 (issue #102; re-done from Luan Antony's PR #121): with GSdx the picture is a child window of gt4-gs.exe, and
// this window keeps the keyboard focus (TASK-038), so an overlay inside gt4-gs, such as ReShade, never saw its keys. The
// keys overlays use for themselves are passed on to that child as well. The game reads its own keys with GetAsyncKeyState
// (host_io.cpp), so it is unaffected; Alt (WM_SYSKEY*) and character messages are never passed on.

#include <windows.h>

namespace gt4::key_forward {

// Home, End, Insert, Delete, Page Up, Page Down and Scroll Lock: ReShade's default overlay key (Home) and the keys overlays
// usually take. None is a default game control (controls.cpp); a player who binds one still drives with it.
constexpr bool forwarded_key(WPARAM key) {
  switch (key) {
    case VK_HOME:
    case VK_END:
    case VK_INSERT:
    case VK_DELETE:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_SCROLL:
      return true;
    default:
      return false;
  }
}

// Only key presses and releases of those keys.
constexpr bool should_forward(UINT message, WPARAM key) {
  return (message == WM_KEYDOWN || message == WM_KEYUP) && forwarded_key(key);
}

}  // namespace gt4::key_forward
