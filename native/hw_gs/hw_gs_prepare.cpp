// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// IN-507: `gt4.exe --prepare-shaders`, run by the launcher before a CoreGS 2.0 session. It starts the renderer on the card
// of the session, without a window or a game, and compiles what the shader cache lacks. The first line on stdout says
// whether there is work, so the launcher shows its window only then:
//   shaders ready             nothing to do (or everything done)
//   shaders progress D T      D of T compiled; `shaders progress 0 T` comes first
//   shaders recorded R        TASK-204: R of those T came from the sessions' own list (variants.bin), and the launcher
//                             logs how many recorded keys it compiled
// Exit 0 when the cache is ready; any other exit leaves the session to compile on its own, as before.
#include "hw_gs_backend.hpp"
#include "hw_gs_options.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <process.h>

namespace gt4::hw {

int prepare_shaders_main() {
  // The pipelines do not depend on the scale, so the renderer's images stay small. A texture pack is not read.
  _putenv_s("GT4_HW_SHADER_PREPARE", "1");
  _putenv_s("GT4_HW_SCALE", "1");
  _putenv_s("GT4_HW_CORE", "2");
  _putenv_s("GT4_HW_TEXTURES", "");
  Options options;
  try {
    options = parse_options([](const char *name) -> const char * { return std::getenv(name); });
  } catch (const OptionError &problem) {
    std::fprintf(stderr, "[shaders] %s\n", problem.what());
    return 5;
  }
  // Never destroyed: the process leaves through _exit once the files are written.
  auto *backend = new Backend;
  std::string error;
  if (!backend->init(options, error)) {
    std::fprintf(stderr, "[shaders] %s\n", error.c_str());
    return 5;
  }
  std::size_t recorded = 0;
  const bool ready = backend->prepare_shaders(
      [](std::size_t done, std::size_t total) {
        std::printf("shaders progress %zu %zu\n", done, total);
        std::fflush(stdout);
      },
      error, &recorded);
  if (!ready) {
    std::fprintf(stderr, "[shaders] %s\n", error.c_str());
    std::fflush(stderr);
    _exit(5);
  }
  if (recorded != 0) {
    std::printf("shaders recorded %zu\n", recorded);
    std::fflush(stdout);
  }
  std::printf("shaders ready\n");
  std::fflush(stdout);
  std::fflush(stderr);
  _exit(0);
}

}  // namespace gt4::hw
