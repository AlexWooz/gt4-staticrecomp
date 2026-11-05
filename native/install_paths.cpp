#include "install_paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include "platform/platform.hpp"  // TASK-180: the loaded game library's folder on Android
#endif

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <system_error>

namespace gt4::install {
namespace {

// The compute shaders of native_vulkan.cpp, one SPIR-V file each.
constexpr const char *kShaderNames[] = {"upload", "copy_read", "copy_write", "load_clut", "display",
                                        "raster", "resolve",   "snapshot",   "decode"};

std::string narrow(const std::filesystem::path &path) { return path.string(); }

} // namespace

std::filesystem::path executable_directory() {
#ifdef _WIN32
  wchar_t path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return {};
  return std::filesystem::path(path).parent_path();
#else
  return gt4::platform::executable_path().parent_path();
#endif
}

std::filesystem::path choose_shader_directory(const std::filesystem::path &build_time_directory) {
  const std::filesystem::path directory = executable_directory();
  if (!directory.empty()) {
    std::error_code error;
    const std::filesystem::path beside_executable = directory / "shaders";
    if (std::filesystem::is_directory(beside_executable, error)) return beside_executable;
    // TASK-081: a release build has no build-time folder. The installed folder is the only answer; the caller
    // reports it as missing.
    if (build_time_directory.empty()) return beside_executable;
  }
  return build_time_directory;
}

bool has_build_time_fallback(const std::filesystem::path &build_time_directory) {
  return !build_time_directory.empty();
}

std::filesystem::path installed_gs_process() {
  const std::filesystem::path directory = executable_directory();
  if (directory.empty()) return {};
  const std::filesystem::path candidate = directory / "gs" / "gt4-gs.exe";
  std::error_code error;
  if (!std::filesystem::is_regular_file(candidate, error)) return {};
  return candidate;
}

int run_portable_check(const std::filesystem::path &build_time_shaders) {
  const std::filesystem::path executable = executable_directory();
  const std::filesystem::path shaders = choose_shader_directory(build_time_shaders);
  std::error_code folder_error;
  const bool installed_shaders =
      shaders == executable / "shaders" && std::filesystem::is_directory(shaders, folder_error);
  const bool fallback = has_build_time_fallback(build_time_shaders);
  std::printf("[portable] executable folder: %s\n", narrow(executable).c_str());
  std::printf("[portable] shader folder: %s (%s)\n", narrow(shaders).c_str(),
              installed_shaders ? "installed, next to the executable"
              : fallback        ? "build-time fallback"
                                : "missing; a release build has no build-time fallback");
  std::printf("[portable] build-time shader fallback: %s\n",
              fallback ? "present (development build)" : "none (release build)");

  int missing = 0;
  for (const char *name : kShaderNames) {
    const std::filesystem::path file = shaders / (std::string(name) + ".spv");
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
      std::printf("[portable]   missing shader: %s\n", narrow(file).c_str());
      ++missing;
    }
  }
  std::printf("[portable] shaders: %s (%zu of %zu present)\n", missing == 0 ? "complete" : "INCOMPLETE",
              std::size(kShaderNames) - static_cast<std::size_t>(missing), std::size(kShaderNames));

  // GT4_GS_PROCESS (set by the launcher or the user) wins over <executable folder>\gs\gt4-gs.exe.
  const char *override_process = std::getenv("GT4_GS_PROCESS");
  if (override_process && *override_process)
    std::printf("[portable] GS process (GT4_GS_PROCESS): %s\n", override_process);
  else if (const std::filesystem::path gs = installed_gs_process(); !gs.empty())
    std::printf("[portable] GS process: %s (installed package)\n", narrow(gs).c_str());
  else
    std::printf("[portable] GS process: none (needed only with GT4_RENDERER=pcsx2)\n");

  std::printf("[portable] memory card: --memcard <file>; no default card path is built in\n");
  std::printf("[portable] result: %s\n", missing == 0 ? "ok" : "shaders missing");
  return missing == 0 ? 0 : 1;
}

} // namespace gt4::install
