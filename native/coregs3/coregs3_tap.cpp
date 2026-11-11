// TASK-090 CoreGS 3.0: the VU1 tap recorder. See coregs3_tap.hpp.
//
// Threading: `tap_path1` runs on the VU1 worker thread (or on the EE thread when the worker is off), `tap_at_vblank` on
// the EE thread and `tap_shutdown` at exit. A single mutex serialises the file write; the cost is a capture-only cost and
// is counted, never on the normal path (the switch is off and every entry point is one branch).
#include "coregs3_tap.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace gt4::coregs3 {
namespace {

struct TapState {
  bool configured = false;
  std::FILE *file = nullptr;
  std::mutex mutex;
  std::atomic<std::uint64_t> vblank{0};
  std::uint64_t first = 0, last = 0;
  bool range = false;
  TapCounters counters;
};

TapState g_tap;
std::atomic<LiveJobSourceSink> g_live_source_sink{nullptr};

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

}  // namespace

void configure_tap_from_environment() {
  if (g_tap.configured) return;
  g_tap.configured = true;
  const char *path = std::getenv("GT4_COREGS3_TAP");
  if (path == nullptr || *path == '\0') return;
  g_tap.file = std::fopen(path, "wb");
  if (g_tap.file == nullptr) {
    std::fprintf(stderr, "[coregs3] cannot open the VU1 tap %s\n", path);
    std::exit(2);
  }
  std::vector<std::uint8_t> header;
  put_u32(header, kTapMagic);
  put_u32(header, kTapVersion);
  put_u32(header, 0u);
  put_u32(header, 0u);
  if (std::fwrite(header.data(), 1, header.size(), g_tap.file) != header.size()) {
    std::fprintf(stderr, "[coregs3] the VU1 tap header could not be written\n");
    std::exit(2);
  }
  g_tap.counters.bytes = header.size();
  // The tap follows the capture window by default: recording every PATH1 emission of a whole drive is 400 MB and helps
  // nobody. GT4_COREGS3_TAP_VBLANKS overrides it.
  const char *range = std::getenv("GT4_COREGS3_TAP_VBLANKS");
  if (range == nullptr || *range == '\0') range = std::getenv("GT4_COREGS3_CAPTURE_VBLANKS");
  if (range != nullptr && *range != '\0') {
    char *end = nullptr;
    const long long first = std::strtoll(range, &end, 10);
    if (end != nullptr && *end == '-') {
      const long long last = std::strtoll(end + 1, nullptr, 10);
      if (first >= 0 && last >= first) {
        g_tap.first = static_cast<std::uint64_t>(first);
        g_tap.last = static_cast<std::uint64_t>(last);
        g_tap.range = true;
      }
    }
  }
  std::fprintf(stderr, "[coregs3] VU1 tap %s (bound %zu MB)\n", path, kTapBytes >> 20);
}

void tap_path1(std::uint64_t job, const std::uint8_t *data, std::size_t bytes) noexcept {
  if (g_tap.file == nullptr || data == nullptr || bytes == 0) return;
  if (bytes % 16 != 0) return;  // a GIF record is whole 128-bit words; anything else is a caller bug
  const std::uint64_t vblank = g_tap.vblank.load(std::memory_order_relaxed);
  if (g_tap.range && (vblank < g_tap.first || vblank > g_tap.last)) return;
  std::lock_guard<std::mutex> lock(g_tap.mutex);
  if (g_tap.counters.bytes + bytes + 24 > kTapBytes) {
    g_tap.counters.refused += 1;
    return;
  }
  std::vector<std::uint8_t> record;
  record.reserve(bytes + 24);
  put_u32(record, static_cast<std::uint32_t>(bytes));
  put_u32(record, kTapKindGif);
  put_u64(record, job);
  put_u64(record, vblank);
  const std::size_t header_bytes = record.size();
  record.insert(record.end(), data, data + bytes);
  if (std::fwrite(record.data(), 1, record.size(), g_tap.file) != record.size()) {
    g_tap.counters.refused += 1;
    return;
  }
  g_tap.counters.records += 1;
  g_tap.counters.bytes += header_bytes + bytes;
}

void tap_job_source(std::uint64_t job, std::uint32_t guest_address, std::uint32_t qwc) noexcept {
  if (g_tap.file == nullptr) return;
  const std::uint64_t vblank_now = g_tap.vblank.load(std::memory_order_relaxed);
  if (g_tap.range && (vblank_now < g_tap.first || vblank_now > g_tap.last)) return;
  std::lock_guard<std::mutex> lock(g_tap.mutex);
  if (g_tap.counters.bytes + 40 > kTapBytes) {
    g_tap.counters.refused += 1;
    return;
  }
  std::vector<std::uint8_t> record;
  put_u32(record, 16u);           // payload: address, qwc, reserved[2]
  put_u32(record, kTapKindSource);
  put_u64(record, job);
  put_u64(record, g_tap.vblank.load(std::memory_order_relaxed));
  put_u32(record, guest_address);
  put_u32(record, qwc);
  put_u32(record, 0u);
  put_u32(record, 0u);
  if (std::fwrite(record.data(), 1, record.size(), g_tap.file) != record.size()) {
    g_tap.counters.refused += 1;
    return;
  }
  g_tap.counters.sources += 1;
  g_tap.counters.bytes += record.size();
}

void tap_at_vblank(std::uint64_t vblank) noexcept { g_tap.vblank.store(vblank, std::memory_order_relaxed); }

void set_live_job_source_sink(LiveJobSourceSink sink) noexcept {
  g_live_source_sink.store(sink, std::memory_order_release);
}

void live_job_source(std::uint64_t job, std::uint32_t guest_address) noexcept {
  const LiveJobSourceSink sink = g_live_source_sink.load(std::memory_order_acquire);
  if (sink != nullptr) sink(job, guest_address);
}

void tap_shutdown() {
  std::lock_guard<std::mutex> lock(g_tap.mutex);
  if (g_tap.file != nullptr) {
    std::fflush(g_tap.file);
    std::fclose(g_tap.file);
    g_tap.file = nullptr;
  }
}

const TapCounters &tap_counters() noexcept { return g_tap.counters; }

}  // namespace gt4::coregs3
