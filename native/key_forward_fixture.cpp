// TASK-154: which key messages the game window passes on to the gt4-gs picture window (key_forward.hpp).

#include "key_forward.hpp"

#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using gt4::key_forward::should_forward;
  for (WPARAM key : {VK_HOME, VK_END, VK_INSERT, VK_DELETE, VK_PRIOR, VK_NEXT, VK_SCROLL}) {
    expect(should_forward(WM_KEYDOWN, key), "an overlay key press is passed on");
    expect(should_forward(WM_KEYUP, key), "its release too, so the overlay never sees it held");
  }
  // Alt combinations and characters stay with the game window (Alt+F4, Alt+Enter, Alt+Space).
  expect(!should_forward(WM_SYSKEYDOWN, VK_HOME), "no WM_SYSKEYDOWN");
  expect(!should_forward(WM_SYSKEYUP, VK_HOME), "no WM_SYSKEYUP");
  expect(!should_forward(WM_CHAR, 'z'), "no WM_CHAR");
  // The game's own keys and the window's F-keys are not passed on.
  for (WPARAM key : {WPARAM{VK_F7}, WPARAM{VK_F8}, WPARAM{VK_F9}, WPARAM{VK_F11}, WPARAM{VK_RETURN}, WPARAM{VK_ESCAPE},
                     WPARAM{VK_UP}, WPARAM{VK_DOWN}, WPARAM{VK_LEFT}, WPARAM{VK_RIGHT}, WPARAM{'Z'}, WPARAM{'A'},
                     WPARAM{'S'}, WPARAM{'X'}}) {
    expect(!should_forward(WM_KEYDOWN, key), "a game or window key stays with the game window");
  }
  if (failures != 0) return 1;
  std::printf("key_forward_fixture: ok\n");
  return 0;
}
