// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-180: the performance HUD's system side outside Windows (hw_hud_system.cpp is the Win32 form: GDI glyphs, PDH
// GPU counters). Android first: no font is drawn yet (an empty atlas: the HUD panel shows no text), and the readings are
// the process CPU share and resident memory from the kernel, twice a second, at the same period. The GPU busy share and
// the refresh rate are not known here (shown as "--").
#include "hw_hud_system.hpp"

#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace gt4::hw::hud {

GlyphAtlas make_glyph_atlas(int pixel_height) {
  (void)pixel_height;
  return {};
}

SystemSampler::SystemSampler(void *window, std::function<void(const SystemReading &)> publish)
    : window_(window), publish_(std::move(publish)), thread_([this] { run(); }) {}

SystemSampler::~SystemSampler() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  stop_changed_.notify_all();
  thread_.join();
}

void SystemSampler::run() {
  const long threads = sysconf(_SC_NPROCESSORS_ONLN);
  const auto process_cpu_s = [] {
    timespec value{};
    return clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0 ? static_cast<double>(value.tv_sec) + value.tv_nsec / 1e9
                                                                 : -1.0;
  };
  double last_cpu = process_cpu_s();
  auto last_wall = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    stop_changed_.wait_for(lock, std::chrono::milliseconds(kPeriodMs));
    if (stop_) break;
    SystemReading reading;
    const double cpu = process_cpu_s();
    const auto wall = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(wall - last_wall).count();
    if (cpu >= 0.0 && last_cpu >= 0.0 && seconds > 0.0 && threads > 0)
      reading.cpu_percent = (cpu - last_cpu) / seconds / static_cast<double>(threads) * 100.0;
    last_cpu = cpu;
    last_wall = wall;
    if (std::FILE *statm = std::fopen("/proc/self/statm", "r")) {
      unsigned long size = 0, resident = 0;
      if (std::fscanf(statm, "%lu %lu", &size, &resident) == 2)
        reading.memory_mb = static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
      std::fclose(statm);
    }
    lock.unlock();
    publish_(reading);
    lock.lock();
  }
}

}  // namespace gt4::hw::hud
