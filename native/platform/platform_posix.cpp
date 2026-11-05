// SPDX-License-Identifier: MIT
// TASK-180: the POSIX form of platform.hpp (Android first; Linux uses the same calls). With GT4_PLATFORM_SDL3 the error
// dialog is SDL's message box; on Android every error also goes to logcat.
#include "platform.hpp"

#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifdef GT4_PLATFORM_SDL3
#include <SDL3/SDL_messagebox.h>
#endif

namespace gt4::platform {
namespace {

std::uint64_t clock_us(clockid_t clock) {
  timespec now{};
  if (clock_gettime(clock, &now) != 0) return UINT64_MAX;
  return static_cast<std::uint64_t>(now.tv_sec) * 1000000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
}

}  // namespace

std::uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

std::optional<std::string> environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) return std::nullopt;
  return std::string(value);
}

std::filesystem::path environment_path(const char *name) {
  const char *value = std::getenv(name);  // UTF-8 on Linux and Android
  return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
}

bool set_environment(const char *name, const char *value) { return setenv(name, value, 1) == 0; }

std::filesystem::path executable_path() {
  // The module that holds this function: the executable on Linux, libgt4.so on Android (whose process is the app).
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&executable_path), &info) != 0 && info.dli_fname != nullptr) {
    std::error_code error;
    const std::filesystem::path path = std::filesystem::absolute(info.dli_fname, error);
    if (!error) return path;
  }
  char path[4096];
  const ssize_t length = readlink("/proc/self/exe", path, sizeof path - 1);
  if (length <= 0) return {};
  path[length] = '\0';
  return std::filesystem::path(path);
}

std::uint64_t thread_cpu_us() { return clock_us(CLOCK_THREAD_CPUTIME_ID); }

std::uint64_t process_cpu_us() { return clock_us(CLOCK_PROCESS_CPUTIME_ID); }

void show_error(const std::string &title, const std::string &text) {
  if (const char *file = std::getenv("GT4_STANDALONE_MESSAGE_FILE"); file != nullptr && *file != '\0') {
    std::ofstream(file, std::ios::binary) << title << "\n" << text << "\n";
    return;
  }
  std::fprintf(stderr, "[error] %s: %s\n", title.c_str(), text.c_str());
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_ERROR, "GT4Recomp", "%s: %s", title.c_str(), text.c_str());
#endif
#ifdef GT4_PLATFORM_SDL3
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
#endif
}

}  // namespace gt4::platform
