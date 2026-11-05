// SPDX-License-Identifier: MIT
// TASK-180: controls of the platform layer (native/platform/platform.hpp). The same checks hold for the Win32 form (CTest
// native_platform) and the POSIX form (the same file built for Linux or Android and run there).
#include "platform.hpp"
#include "cycle_counter.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
int failures = 0;

void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main(int, char **argv) {
  namespace platform = gt4::platform;

  // Environment: set, read back as text and as a path, and unset names read as missing.
  require(platform::set_environment("GT4_PLATFORM_FIXTURE", "value=1 two"), "set_environment succeeds");
  const std::optional<std::string> value = platform::environment("GT4_PLATFORM_FIXTURE");
  require(value && *value == "value=1 two", "environment reads back the value set");
  require(platform::environment_path("GT4_PLATFORM_FIXTURE") == std::filesystem::path("value=1 two"),
          "environment_path reads back the value set");
  require(!platform::environment("GT4_PLATFORM_FIXTURE_UNSET"), "an unset name is missing");
  require(platform::environment_path("GT4_PLATFORM_FIXTURE_UNSET").empty(), "an unset path is empty");

  // Clocks: the monotonic milliseconds never go back; CPU time is known and grows with work; the cycle counter moves.
  const std::uint64_t first_ms = platform::monotonic_ms();
  const std::uint64_t first_cpu = platform::thread_cpu_us();
  const std::uint64_t first_cycles = platform::cycle_counter();
  volatile std::uint64_t sink = 0;
  while (platform::monotonic_ms() < first_ms + 30) sink = sink + 1;  // about 30 ms of work on this thread
  require(platform::monotonic_ms() >= first_ms + 30, "monotonic_ms advances");
  // Windows counts thread CPU time in scheduler ticks (about 15.6 ms): work until one passes, two seconds at most.
  while (platform::thread_cpu_us() == first_cpu && platform::monotonic_ms() < first_ms + 2000) sink = sink + 1;
  require(first_cpu != UINT64_MAX && platform::thread_cpu_us() > first_cpu, "thread CPU time is known and grows");
  require(platform::process_cpu_us() != UINT64_MAX && platform::process_cpu_us() >= platform::thread_cpu_us() / 2,
          "process CPU time is known");
  require(platform::cycle_counter() > first_cycles, "the cycle counter advances");

  // The executable is this fixture.
  const std::filesystem::path executable = platform::executable_path();
  require(!executable.empty() && std::filesystem::exists(executable), "executable_path names an existing file");
  require(executable.stem() == std::filesystem::path(argv[0]).stem(), "executable_path is this program");

  // The error box goes to the test file when GT4_STANDALONE_MESSAGE_FILE is set (headless checks).
  const std::filesystem::path message = std::filesystem::temp_directory_path() / "gt4-platform-fixture-message.txt";
  std::error_code ignored;
  std::filesystem::remove(message, ignored);
  platform::set_environment("GT4_STANDALONE_MESSAGE_FILE", message.string().c_str());
  platform::show_error("Title", "Text");
  std::ifstream file(message, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  require(text == "Title\nText\n", "show_error writes title and text to GT4_STANDALONE_MESSAGE_FILE");
  file.close();
  std::filesystem::remove(message, ignored);

  if (failures == 0) std::printf("PASS: platform layer\n");
  return failures == 0 ? 0 : 1;
}
