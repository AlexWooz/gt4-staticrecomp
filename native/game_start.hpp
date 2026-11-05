// SPDX-License-Identifier: MIT
// TASK-180: the platform-free part of the game's start, shared by the Windows entry point (game_main.cpp, which adds the
// standalone double-click start) and the Android entry point (android_main.cpp, the SDL3 game activity).
#pragma once

// The pinned donor main (port main.cpp compiled with `main` renamed, native/CMakeLists.txt).
int gt4_donor_main(int argc, char **argv);

namespace gt4::start {

// TASK-116 step 0 (P0 gate): gives the GS stream client the guest RAM window that turns a READ_FIFO host pointer into an
// EE address in the GT4_GS_READBACK_LOG lines. Nothing happens when the switch is unset or the build has no GS stream.
void note_guest_ram();

// TASK-161: one switch drives both sides of the full trigger range: the game's pedal scale (the translated statements
// read gt4::trigger_range) and the controls' trigger ranges (the whole byte with it on, 45 and 84 with it off), set
// before any pad is read, so the launcher and a direct start cannot disagree.
void configure_trigger_range();

// TASK-159/160/161: the game options that arrive as environment values (camera_360, fov_adjust, trigger_full_range). An
// invalid value stops the start here.
void configure_game_options();

// Explicit start: a command line with the ELF argument (the launcher, the harnesses, the pins, the Android activity).
// Straight into the donor main after the options; a headless run (no --window/--fullscreen) installs the input replay.
int run_explicit(int argc, char **argv);

}  // namespace gt4::start
