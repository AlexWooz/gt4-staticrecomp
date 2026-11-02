#pragma once
#include <functional>

bool open_window(bool fullscreen);
bool install_input_replay_without_window();  // IN-520: GT4_INPUT_REPLAY on a headless run
int run_window(const std::function<int()> &emulate);
