// SPDX-License-Identifier: MIT
// TASK-180: the Win32 form of platform.hpp. Each function is the call the Windows build made before the seam existed
// (game_main.cpp, install_paths.cpp, host_io.cpp, gs_stream_client.cpp), so the Windows behaviour is unchanged.
#include "platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace gt4::platform {
namespace {

std::uint64_t filetime_ticks(const FILETIME &value) {
  return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

// The message box text in the ANSI code page, as game_main.cpp always converted it.
std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

}  // namespace

std::uint64_t monotonic_ms() { return GetTickCount64(); }

std::optional<std::string> environment(const char *name) {
  char *value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
  std::string text(value);
  std::free(value);
  return text;
}

std::filesystem::path environment_path(const char *name) {
  const std::wstring wide(name, name + std::char_traits<char>::length(name));  // variable names are ASCII
  const wchar_t *value = _wgetenv(wide.c_str());
  return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
}

bool set_environment(const char *name, const char *value) { return _putenv_s(name, value) == 0; }

std::filesystem::path executable_path() {
  wchar_t path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return length > 0 && length < std::size(path) ? std::filesystem::path(path) : std::filesystem::path();
}

std::uint64_t thread_cpu_us() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return UINT64_MAX;
  return filetime_ticks(kernel) / 10 + filetime_ticks(user) / 10;
}

std::uint64_t process_cpu_us() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return UINT64_MAX;
  return filetime_ticks(kernel) / 10 + filetime_ticks(user) / 10;
}

void show_error(const std::string &title, const std::string &text) {
  // Test knob: GT4_STANDALONE_MESSAGE_FILE=<path> writes the text there instead of opening a message box (headless checks).
  char *file = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&file, &length, "GT4_STANDALONE_MESSAGE_FILE") == 0 && file != nullptr && *file != '\0') {
    std::ofstream(file, std::ios::binary) << title << "\n" << text << "\n";
    std::free(file);
    return;
  }
  std::free(file);
  MessageBoxW(nullptr, widen(text).c_str(), widen(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

}  // namespace gt4::platform
