// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: see hw_hud_system.hpp.
#include "hw_hud_system.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace gt4::hw::hud {
namespace {

// The counters of the graphics card, per process and per engine (or per adapter, for the memory). pdh.dll is loaded only when the
// HUD is on, so a game with the HUD off never touches it.
class GpuCounters {
public:
  GpuCounters() {
    library_ = LoadLibraryW(L"pdh.dll");
    if (library_ == nullptr) return;
    open_ = reinterpret_cast<decltype(open_)>(GetProcAddress(library_, "PdhOpenQueryW"));
    add_ = reinterpret_cast<decltype(add_)>(GetProcAddress(library_, "PdhAddEnglishCounterW"));
    collect_ = reinterpret_cast<decltype(collect_)>(GetProcAddress(library_, "PdhCollectQueryData"));
    read_ = reinterpret_cast<decltype(read_)>(GetProcAddress(library_, "PdhGetFormattedCounterArrayW"));
    close_ = reinterpret_cast<decltype(close_)>(GetProcAddress(library_, "PdhCloseQuery"));
    if (!open_ || !add_ || !collect_ || !read_ || !close_ || open_(nullptr, 0, &query_) != ERROR_SUCCESS) {
      query_ = nullptr;
      return;
    }
    const std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    prefix_ = prefix;
    const std::wstring engine_path = L"\\GPU Engine(" + prefix + L"*)\\Utilization Percentage";
    const std::wstring memory_path = L"\\GPU Process Memory(" + prefix + L"*)\\Dedicated Usage";
    if (add_(query_, engine_path.c_str(), 0, &engine_) != ERROR_SUCCESS) engine_ = nullptr;
    if (add_(query_, memory_path.c_str(), 0, &memory_) != ERROR_SUCCESS) memory_ = nullptr;
    collect_(query_);  // the engine's percentage needs two collections
  }

  ~GpuCounters() {
    if (query_ != nullptr) close_(query_);
    if (library_ != nullptr) FreeLibrary(library_);
  }

  void read(SystemReading &out) {
    if (query_ == nullptr || collect_(query_) != ERROR_SUCCESS) return;
    if (engine_ != nullptr) {
      // As Task Manager: the use of the busiest engine.
      double busiest = -1.0;
      for_each_value(engine_, [&](const std::wstring &, double value) { busiest = std::max(busiest, value); });
      if (busiest >= 0.0) out.gpu_percent = std::min(busiest, 100.0);
    }
    if (memory_ != nullptr) {
      double bytes = -1.0;
      for_each_value(memory_, [&](const std::wstring &, double value) { bytes = std::max(bytes, 0.0) + value; });
      if (bytes >= 0.0) out.video_memory_mb = bytes / (1024.0 * 1024.0);
    }
  }

private:
  template <typename Visit> void for_each_value(PDH_HCOUNTER counter, Visit visit) {
    DWORD bytes = 0, count = 0;
    if (read_(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return;
    buffer_.resize(bytes);
    auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer_.data());
    if (read_(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
      const std::wstring name = items[i].szName;
      if (name.rfind(prefix_, 0) == 0) visit(name, items[i].FmtValue.doubleValue);
    }
  }

  HMODULE library_ = nullptr;
  PDH_STATUS(WINAPI *open_)(LPCWSTR, DWORD_PTR, PDH_HQUERY *) = nullptr;
  PDH_STATUS(WINAPI *add_)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *) = nullptr;
  PDH_STATUS(WINAPI *collect_)(PDH_HQUERY) = nullptr;
  PDH_STATUS(WINAPI *read_)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W) = nullptr;
  PDH_STATUS(WINAPI *close_)(PDH_HQUERY) = nullptr;
  PDH_HQUERY query_ = nullptr;
  PDH_HCOUNTER engine_ = nullptr, memory_ = nullptr;
  std::wstring prefix_;
  std::vector<unsigned char> buffer_;
};

std::uint64_t process_cpu_100ns() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto value = [](const FILETIME &time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return value(kernel) + value(user);
}

std::uint32_t refresh_hz_of(HWND window) {
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof info;
  if (!GetMonitorInfoW(monitor, &info)) return 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
  return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
}

}  // namespace

GlyphAtlas make_glyph_atlas(int pixel_height) {
  GlyphAtlas atlas;
  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) return atlas;
  HFONT font = CreateFontW(-pixel_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
  TEXTMETRICW metrics{};
  const constexpr int glyphs = GlyphAtlas::kLast - GlyphAtlas::kFirst + 1;
  if (font != nullptr && GetTextMetricsW(dc, &metrics) && metrics.tmAveCharWidth > 0 && metrics.tmHeight > 0) {
    // Each glyph is drawn apart from the others, with room around it: Consolas' round glyphs reach past their advance (owner,
    // 2026-11-02: the "1" had a stroke in front of it, the edge of the "0" drawn beside it). The cell is then as wide as the ink needs.
    const int advance = metrics.tmAveCharWidth, cell_height = metrics.tmHeight;
    const int room = cell_height, pitch = advance + 2 * room;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = pitch * glyphs;
    info.bmiHeader.biHeight = -cell_height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
      HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      RECT all{0, 0, pitch * glyphs, cell_height};
      FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      for (int i = 0; i < glyphs; ++i) {
        const wchar_t glyph = static_cast<wchar_t>(GlyphAtlas::kFirst + i);
        TextOutW(dc, i * pitch + room, 0, &glyph, 1);
      }
      GdiFlush();
      const auto *pixels = static_cast<const std::uint32_t *>(bits);
      const auto ink_at = [&](int glyph, int x, int y) {
        return static_cast<std::uint8_t>(pixels[static_cast<std::size_t>(y) * (static_cast<std::size_t>(pitch) * glyphs) + static_cast<std::size_t>(glyph * pitch + x)] & 0xFFu);
      };
      // How far the ink of any glyph reaches, from the left of its slot.
      int first_ink = room, last_ink = room + advance - 1;
      for (int i = 0; i < glyphs; ++i) {
        for (int y = 0; y < cell_height; ++y) {
          for (int x = 0; x < pitch; ++x) {
            if (ink_at(i, x, y) != 0) {
              first_ink = std::min(first_ink, x);
              last_ink = std::max(last_ink, x);
            }
          }
        }
      }
      const int cell_width = last_ink - first_ink + 1;
      atlas.cell_width = cell_width;
      atlas.cell_height = cell_height;
      atlas.coverage.resize(static_cast<std::size_t>(cell_width) * glyphs * cell_height);
      std::size_t out = 0;
      for (int y = 0; y < cell_height; ++y) {
        for (int i = 0; i < glyphs; ++i) {
          for (int x = 0; x < cell_width; ++x) atlas.coverage[out++] = ink_at(i, first_ink + x, y);
        }
      }
      SelectObject(dc, old_bitmap);
    }
    if (bitmap != nullptr) DeleteObject(bitmap);
  }
  if (old_font != nullptr) SelectObject(dc, old_font);
  if (font != nullptr) DeleteObject(font);
  DeleteDC(dc);
  return atlas;
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
  // The HUD must not take time from the game.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  GpuCounters gpu;
  const DWORD threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  auto last_time = std::chrono::steady_clock::now();
  std::uint64_t last_cpu = process_cpu_100ns();
  std::uint64_t readings = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_changed_.wait_for(lock, std::chrono::milliseconds(kPeriodMs), [this] { return stop_; })) {
    lock.unlock();
    SystemReading reading;
    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t cpu = process_cpu_100ns();
    const double wall_100ns = std::chrono::duration<double>(now - last_time).count() * 1e7;
    if (wall_100ns > 0.0 && threads > 0) {
      reading.cpu_percent = std::min(100.0, static_cast<double>(cpu - last_cpu) / (wall_100ns * threads) * 100.0);
    }
    last_time = now;
    last_cpu = cpu;
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof memory;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory)) {
      reading.memory_mb = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
    }
    gpu.read(reading);
    reading.refresh_hz = refresh_hz_of(static_cast<HWND>(window_));
    publish_(reading);
    // In the log every 40 readings (the game may end without destructors).
    if (++readings % 40 == 0) {
      FILETIME created, exited, kernel, user;
      if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const auto value = [](const FILETIME &time) {
          return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        std::fprintf(stderr, "[hud] sampler: %llu readings, %.2f ms of processor per reading\n", static_cast<unsigned long long>(readings),
                     static_cast<double>(value(kernel) + value(user)) / 10000.0 / static_cast<double>(readings));
      }
    }
    lock.lock();
  }
}

}  // namespace gt4::hw::hud
