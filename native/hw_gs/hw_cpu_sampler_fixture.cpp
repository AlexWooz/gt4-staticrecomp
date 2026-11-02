// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-027 CPU survey: the GT4_CPU_SAMPLER sampler on a known busy thread. A named thread spins in a function that is
// not inlined; the report must hold samples of that thread whose leaf lies in the function and whose stacks were
// walked past it (the copied-stack unwind), only inside the VBlank window, and an idle thread must not be sampled.

#include "hw_cpu_sampler.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
void require(bool ok, const char *what) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

std::atomic<bool> g_stop{false};
volatile std::uint64_t g_sink = 0;

__declspec(noinline) std::uint64_t spin_in_known_function() {
  std::uint64_t x = 1;
  while (!g_stop.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 4096; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_sink = x;
  }
  return x;
}

__declspec(noinline) void busy_thread_body() {
  gt4::hw::sampler::name_current_thread("busy");
  g_sink = spin_in_known_function();
}

}  // namespace

int main() {
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  const std::string path = std::string(temp) + "gt4_cpu_sampler_fixture.txt";
  _putenv_s("GT4_CPU_SAMPLER", path.c_str());
  _putenv_s("GT4_CPU_SAMPLER_FROM", "10");
  _putenv_s("GT4_CPU_SAMPLER_TO", "1000000");
  require(gt4::hw::sampler::start_from_env(), "the sampler starts when GT4_CPU_SAMPLER is set");

  std::atomic<bool> idle_stop{false};
  std::thread idle([&] {
    gt4::hw::sampler::name_current_thread("idle");
    while (!idle_stop.load()) Sleep(50);
  });
  std::thread busy(busy_thread_body);
  // VBlanks 0..9 are outside the window, then 10.. inside: about 400 ms of sampling.
  for (std::uint64_t v = 0; v < 60; ++v) {
    gt4::hw::sampler::note_vblank(v);
    Sleep(v < 10 ? 20 : 8);
  }
  g_stop = true;
  busy.join();
  idle_stop = true;
  idle.join();
  gt4::hw::sampler::finish(stdout);

  std::ifstream in(path);
  require(static_cast<bool>(in), "the sampler wrote its file");
  std::string line;
  std::uint64_t busy_tid = 0, idle_tid = 0, busy_samples = 0, idle_samples = 0;
  std::uint64_t leaf_hits = 0, deep_hits = 0;
  std::vector<std::string> stack_lines;
  bool header = false;
  const auto spin = reinterpret_cast<std::uint64_t>(&spin_in_known_function);
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string kind;
    fields >> kind;
    if (kind == "gt4-cpu-sampler") header = true;
    if (kind == "thread") {
      std::uint64_t tid = 0, start = 0, cycles = 0, samples = 0;
      double cpu = 0;
      std::string name;
      fields >> tid >> std::hex >> start >> std::dec >> cycles >> samples >> cpu >> name;
      if (name == "busy") busy_tid = tid, busy_samples = samples;
      if (name == "idle") idle_tid = tid, idle_samples = samples;
    }
    if (kind == "s") stack_lines.push_back(line);
  }
  for (const std::string &s : stack_lines) {
    std::istringstream fields(s);
    std::string kind;
    std::uint64_t tid = 0, count = 0, cycles = 0;
    fields >> kind >> tid >> count >> cycles;
    if (tid != busy_tid) continue;
    std::vector<std::uint64_t> frames;
    std::uint64_t address = 0;
    while (fields >> std::hex >> address) frames.push_back(address);
    if (!frames.empty() && frames[0] >= spin && frames[0] < spin + 512) leaf_hits += count;
    if (frames.size() >= 3) deep_hits += count;
  }
  std::printf("busy samples=%llu leaf_in_function=%llu deep=%llu idle samples=%llu\n",
              static_cast<unsigned long long>(busy_samples), static_cast<unsigned long long>(leaf_hits),
              static_cast<unsigned long long>(deep_hits), static_cast<unsigned long long>(idle_samples));
  require(header, "the file starts with the sampler header");
  require(busy_tid != 0 && idle_tid != 0, "both named threads are in the thread table");
  require(busy_samples >= 100, "the busy thread was sampled about once per millisecond in the window");
  require(leaf_hits * 10 >= busy_samples * 9, "at least 90% of its samples have their leaf in the spinning function");
  require(deep_hits * 10 >= busy_samples * 9, "at least 90% of its stacks were walked past the leaf");
  require(idle_samples == 0, "a sleeping thread is never sampled");
  std::remove(path.c_str());
  std::printf(g_failures == 0 ? "hw_cpu_sampler: all checks passed\n" : "hw_cpu_sampler: %d check(s) failed\n",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
