// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// IN-507: `gt4.exe --prepare-shaders`, run by the launcher before a CoreGS 2.0 session. It starts the renderer on the card
// of the session, without a window or a game, and compiles what the shader cache lacks. The first line on stdout says
// whether there is work, so the launcher shows its window only then:
//   shaders scale N           TASK-206: the render scale the list was expanded for (1 when the option is absent)
//   shaders ready             nothing to do (or everything done)
//   shaders progress D T      D of T compiled; `shaders progress 0 T` comes first
//   shaders recorded R        TASK-204: R of those T came from the sessions' own list (variants.bin), and the launcher
//                             logs how many recorded keys it compiled
//   shaders added K           TASK-206: K keys the scale expansion added beyond the 1x shapes (absent at 1x)
// Exit 0 when the cache is ready; any other exit leaves the session to compile on its own, as before.
//
// TASK-206: `--prepare-shaders --prepare-scale N` prepares the list for a session at render scale N (the CoreGS 2.0
// internal multiplier, GT4_HW_SCALE, not the Render scale percentage). A stand-alone `--prepare-shaders` without the
// option prepares scale 1 and says so: that is the old behaviour.
#include "hw_gs_backend.hpp"
#include "hw_gs_options.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <process.h>

namespace gt4::hw {
namespace {

constexpr const char *kPrepareScaleOption = "--prepare-scale";

// The scale of `--prepare-shaders`: 1 without the option. Anything malformed is an error, never a silent default (project
// rule).
bool parse_prepare_scale(int argumentCount, const char *const *arguments, std::uint32_t &scale, std::string &error) {
  scale = 1;
  for (int i = 0; i < argumentCount; ++i) {
    const char *argument = arguments[i];
    if (std::strcmp(argument, kPrepareScaleOption) != 0) {
      error = std::string("unknown argument \"") + argument + "\" (usage: --prepare-shaders [--prepare-scale 1..12])";
      return false;
    }
    if (i + 1 >= argumentCount) {
      error = "--prepare-scale needs the render scale (1..12)";
      return false;
    }
    const char *value = arguments[++i];
    char *end = nullptr;
    const long number = std::strtol(value, &end, 10);
    if (value[0] == '\0' || *end != '\0' || number < 1 || number > 12) {
      error = std::string("--prepare-scale: invalid value \"") + value + "\" (expected 1..12, the internal multiplier)";
      return false;
    }
    scale = static_cast<std::uint32_t>(number);
  }
  return true;
}

}  // namespace

int prepare_shaders_main(int argumentCount, const char *const *arguments) {
  std::uint32_t scale = 1;
  std::string problem;
  if (!parse_prepare_scale(argumentCount, arguments, scale, problem)) {
    std::fprintf(stderr, "[shaders] %s\n", problem.c_str());
    return 5;
  }
  // The renderer starts at the session's scale because its variant list is the list a session at that scale asks for
  // (TASK-206); the pipelines themselves do not depend on the scale. A texture pack is not read.
  char scaleText[8];
  std::snprintf(scaleText, sizeof scaleText, "%u", scale);
  _putenv_s("GT4_HW_SHADER_PREPARE", "1");
  _putenv_s("GT4_HW_SCALE", scaleText);
  _putenv_s("GT4_HW_CORE", "2");
  _putenv_s("GT4_HW_TEXTURES", "");
  std::printf("shaders scale %u\n", scale);
  std::fflush(stdout);
  std::fprintf(stderr, "[shaders] preparing the variants of the %ux render scale%s\n", scale,
               argumentCount == 0 ? " (--prepare-scale absent: the old behaviour)" : "");
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
  std::size_t added = 0;
  const bool ready = backend->prepare_shaders(
      [](std::size_t done, std::size_t total) {
        std::printf("shaders progress %zu %zu\n", done, total);
        std::fflush(stdout);
      },
      error, &recorded, &added);
  if (!ready) {
    std::fprintf(stderr, "[shaders] %s\n", error.c_str());
    std::fflush(stderr);
    _exit(5);
  }
  if (recorded != 0) {
    std::printf("shaders recorded %zu\n", recorded);
    std::fflush(stdout);
  }
  if (added != 0) {
    std::printf("shaders added %zu\n", added);
    std::fflush(stdout);
  }
  std::printf("shaders ready\n");
  std::fflush(stdout);
  std::fflush(stderr);
  _exit(0);
}

}  // namespace gt4::hw
