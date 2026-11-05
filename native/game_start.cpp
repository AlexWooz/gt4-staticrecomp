// SPDX-License-Identifier: MIT
// TASK-180: see game_start.hpp. Moved unchanged from game_main.cpp (TASK-097), which keeps the Windows-only standalone
// start.
#include "game_start.hpp"

#include "camera_360.hpp"
#include "controls.hpp"
#include "fov_adjust.hpp"
#include "frame_skip.hpp"
#include "trigger_range.hpp"
#include "window.hpp"

#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#include "ps2/memory.hpp"
#endif

#include <cstring>

namespace gt4::start {

void note_guest_ram() {
#ifdef GT4_GS_STREAM_CLIENT
  gt4::gs_stream::set_guest_ram(ps2::g_ram, ps2::kRamSize);
#endif
}

void configure_trigger_range() {
  gt4::trigger_range::configure_from_environment();
  gt4::controls::set_pressure_scale(gt4::trigger_range::enabled() ? gt4::controls::PressureScale::full
                                                                  : gt4::controls::PressureScale::stock);
}

void configure_game_options() {
  gt4::camera360::configure_from_environment();  // TASK-159: game.ini camera_360 arrives here as GT4_CAMERA_360
  gt4::fov_adjust::configure_from_environment();  // TASK-160: game.ini fov_adjust arrives here as GT4_FOV_ADJUST
  configure_trigger_range();                       // TASK-161: game.ini trigger_full_range as GT4_TRIGGER_FULL_RANGE
}

int run_explicit(int argc, char **argv) {
  // IN-520: the window sets up the recording and replay files itself; a headless run replays here.
  bool window = false;
  for (int i = 1; i < argc; ++i) {
    window = window || std::strcmp(argv[i], "--window") == 0 || std::strcmp(argv[i], "--fullscreen") == 0;
  }
  gt4::frame_skip::start_from_environment();
  configure_game_options();  // an invalid GT4_CAMERA_360, GT4_FOV_ADJUST or GT4_TRIGGER_FULL_RANGE stops the start here
  if (!window && !install_input_replay_without_window()) return 1;
  return gt4_donor_main(argc, argv);
}

}  // namespace gt4::start
