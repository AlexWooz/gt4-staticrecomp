// SPDX-License-Identifier: MIT
// TASK-180 (docs/LINUX-PORT-PLAN.md stage L0, docs/ANDROID-PORT-PLAN.md stage A2): the platform layer of the game.
//
// Process-wide helpers that differ between Windows and the other targets. Each has a Win32 form (platform_win32.cpp,
// the same calls the Windows build always made) and a POSIX form (platform_posix.cpp, Linux and Android). The larger
// platform pieces have their own seams:
//   - the window, its events and the VBlank hook: window.hpp (window.cpp on Win32, window_sdl.cpp with SDL3);
//   - audio output and pads: host_io.hpp (WinMM and XInput in host_io.cpp, SDL3 audio in audio_sdl.cpp; SDL3 pads on
//     every platform in gamepad_sdl.cpp);
//   - the frame clock: frame_clock.hpp (frame_clock.cpp on Win32, frame_clock_posix.cpp elsewhere);
//   - the Vulkan surface: the renderers' own loaders (CoreGS 2.0: hw_gs/hw_gs_vulkan_loader.*).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace gt4::platform {

// Milliseconds of a monotonic clock (GetTickCount64 on Windows, CLOCK_MONOTONIC elsewhere). Only differences matter.
std::uint64_t monotonic_ms();

// The value of `name` in the process environment, or nullopt when it is not set.
std::optional<std::string> environment(const char *name);

// The value of `name` as a path (wide on Windows, so non-ASCII folders survive), or an empty path when it is not set.
std::filesystem::path environment_path(const char *name);

// Sets `name` to `value` in the process environment that std::getenv reads. False when the C runtime refused.
bool set_environment(const char *name, const char *value);

// The path of the running executable (Windows) or of the loaded game library (Android); empty when unknown.
std::filesystem::path executable_path();

// CPU time of the calling thread and of the whole process, in microseconds; UINT64_MAX when unknown.
std::uint64_t thread_cpu_us();
std::uint64_t process_cpu_us();

// The player-facing error dialog: a message box on Windows, SDL's message box in SDL3 builds (stderr when neither can
// be shown). GT4_STANDALONE_MESSAGE_FILE=<path> writes the text there instead (headless checks), on every platform.
void show_error(const std::string &title, const std::string &text);

}  // namespace gt4::platform
