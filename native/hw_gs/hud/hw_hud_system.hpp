// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
//
// TASK-135: what the HUD asks Windows: the font of its text, and the game's use of the machine. Nothing here runs per frame: the font
// is drawn once, and SystemSampler reads the machine on a thread of its own, about twice a second, while the HUD is on.
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "hw_hud.hpp"

namespace gt4::hw::hud {

// The glyphs of Windows' monospaced Consolas at this height in pixels. Empty (`coverage`) when Windows cannot draw them.
GlyphAtlas make_glyph_atlas(int pixel_height);

class SystemSampler {
public:
  static constexpr int kPeriodMs = 500;

  // `window` (HWND, may be null): whose monitor the refresh rate is read from.
  SystemSampler(void *window, std::function<void(const SystemReading &)> publish);
  ~SystemSampler();
  SystemSampler(const SystemSampler &) = delete;
  SystemSampler &operator=(const SystemSampler &) = delete;

private:
  void run();

  void *window_;
  std::function<void(const SystemReading &)> publish_;
  std::mutex mutex_;
  std::condition_variable stop_changed_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace gt4::hw::hud
