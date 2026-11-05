// SPDX-License-Identifier: MIT
// TASK-180 (Android plan A2): the entry point of libgt4.so, called by SDL3's SDLActivity on its game thread (SDL_main).
//
// First-run layout (no Storage Access Framework yet, docs/ANDROID-EMULATOR.md): the player's files are pushed with adb
// into the app-specific external folder, /sdcard/Android/data/<package>/files/:
//   SCUS_973.28.elf   the reconstructed game ELF (the launcher's setup makes it on Windows)
//   disc.iso          the player's own disc image (Gran Turismo 4 USA v2.00)
//   card.ps2          the memory card (created by the game when missing)
//   gt4.env           optional: NAME=VALUE lines set in the environment before the game starts (game.ini values as the
//                     launcher passes them, for example GT4_HW_SCALE=1)
//   args.txt          optional: extra command-line arguments, one per line (for example --frames 3000 for a bounded run)
// The game writes game.log there (stderr, also copied to logcat under the tag GT4Recomp).
// Defaults: CoreGS 2.0 (GT4_RENDERER=hw, GT4_HW_CORE=2), 1x, 16:9, the race display at 480p, paced frames; the same
// defaults the launcher gives a CoreGS 2.0 start on Windows.
#include "game_start.hpp"
#include "platform/platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kLogTag = "GT4Recomp";

// stdout and stderr go to <files>/game.log and to logcat: a pipe read by a thread that writes both.
void redirect_output(const std::filesystem::path &log_path) {
  int pipe_ends[2];
  if (pipe(pipe_ends) != 0) return;
  std::FILE *log = std::fopen(log_path.c_str(), "w");
  dup2(pipe_ends[1], STDOUT_FILENO);
  dup2(pipe_ends[1], STDERR_FILENO);
  close(pipe_ends[1]);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  std::thread([read_end = pipe_ends[0], log] {
    std::string line;
    char buffer[4096];
    for (;;) {
      const ssize_t got = read(read_end, buffer, sizeof buffer);
      if (got <= 0) break;
      if (log != nullptr) {
        std::fwrite(buffer, 1, static_cast<std::size_t>(got), log);
        std::fflush(log);
      }
      for (ssize_t i = 0; i < got; ++i) {
        if (buffer[i] == '\n') {
          __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
          line.clear();
        } else {
          line.push_back(buffer[i]);
        }
      }
    }
  }).detach();
}

void set_default(const char *name, const char *value) {
  if (std::getenv(name) == nullptr) gt4::platform::set_environment(name, value);
}

// NAME=VALUE lines; '#' starts a comment line. Values replace the defaults above.
void apply_environment_file(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0) {
      std::fprintf(stderr, "[android] %s: ignored line without NAME=VALUE: %s\n", path.c_str(), line.c_str());
      continue;
    }
    const std::string name = line.substr(0, equals), value = line.substr(equals + 1);
    gt4::platform::set_environment(name.c_str(), value.c_str());
    std::fprintf(stderr, "[android] %s=%s\n", name.c_str(), value.c_str());
  }
}

std::vector<std::string> read_arguments(const std::filesystem::path &path) {
  std::vector<std::string> out;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line[0] != '#') out.push_back(line);
  }
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *external = SDL_GetAndroidExternalStoragePath();
  if (external == nullptr) {
    gt4::platform::show_error("Gran Turismo 4", std::string("No app storage folder: ") + SDL_GetError());
    return 3;
  }
  const std::filesystem::path root(external);
  redirect_output(root / "game.log");
  std::fprintf(stderr, "[android] GT4Recomp game library, files in %s\n", root.c_str());
  // The EE runs on this thread (SDL's game thread): its stack size matters for the large translated functions (Android
  // plan risk 7; Windows runs it on the main thread's 1 MB).
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    std::size_t stack_bytes = 0;
    pthread_attr_getstacksize(&attributes, &stack_bytes);
    pthread_attr_destroy(&attributes);
    std::fprintf(stderr, "[android] game thread stack %zu KiB\n", stack_bytes / 1024);
  }
  std::error_code ignored;
  std::filesystem::current_path(root, ignored);

  set_default("GT4_RENDERER", "hw");
  set_default("GT4_HW_CORE", "2");
  set_default("GT4_HW_SCALE", "1");
  set_default("GT4_FORCE_480P", "1");
  set_default("GT4_FORCE_WIDESCREEN", "1");
  set_default("GT4_DISPLAY_ASPECT", "auto");
  set_default("GT4_PRESENT_MODE", "fifo");
  set_default("GT4_HW_SHADER_CACHE", (root / "shader-cache").c_str());
  apply_environment_file(root / "gt4.env");

  const std::filesystem::path elf = root / "SCUS_973.28.elf", disc = root / "disc.iso", card = root / "card.ps2";
  for (const std::filesystem::path &needed : {elf, disc}) {
    if (!std::filesystem::exists(needed, ignored)) {
      gt4::platform::show_error("Gran Turismo 4", "Missing " + needed.string() +
                                                      ".\nPush the game files with adb first (docs/ANDROID-EMULATOR.md).");
      return 3;
    }
  }
  std::vector<std::string> storage = {"libgt4.so", elf.string(), "--ntsc", "--disc", disc.string(), "--frames", "0",
                                      "--memcard", card.string(), "--misses", (root / "misses.txt").string(),
                                      "--fullscreen"};
  for (std::string &extra : read_arguments(root / "args.txt")) storage.push_back(std::move(extra));
  std::vector<char *> pointers;
  for (std::string &argument : storage) {
    std::fprintf(stderr, "[android] argument %s\n", argument.c_str());
    pointers.push_back(argument.data());
  }
  pointers.push_back(nullptr);
  gt4::start::note_guest_ram();
  const int result = gt4::start::run_explicit(static_cast<int>(storage.size()), pointers.data());
  std::fprintf(stderr, "[android] the game ended with %d\n", result);
  return result;
}
