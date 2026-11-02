#define _CRT_SECURE_NO_WARNINGS
#include "frame_skip.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace gt4::frame_skip {

namespace {

std::uint32_t read_word(const std::uint8_t *ram, std::size_t ramSize, std::uint32_t address) {
  if (static_cast<std::size_t>(address) + 4 > ramSize) return 0;
  std::uint32_t value = 0;
  std::memcpy(&value, ram + address, sizeof value);
  return value;
}

void write_word(std::uint8_t *ram, std::size_t ramSize, std::uint32_t address, std::uint32_t value) {
  if (static_cast<std::size_t>(address) + 4 > ramSize) return;
  std::memcpy(ram + address, &value, sizeof value);
}

std::uint64_t fnv1a(const std::uint8_t *data, std::size_t bytes) {
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

void Controller::configure(const Guest &guest, int forcePattern, bool adaptive, std::FILE *log) {
  guest_ = guest;
  forcePattern_ = forcePattern;
  adaptive_ = adaptive;
  log_ = log;
}

bool Controller::is_race(std::uint32_t object, const std::uint8_t *ram, std::size_t ramSize) const {
  if (object == 0 || object >= ramSize) return false;
  const std::uint32_t vtable = read_word(ram, ramSize, object + 0x64u);
  if (vtable < guest_.raceVtableFirst || vtable > guest_.raceVtableLast) return false;
  const std::uint32_t dynamics = read_word(ram, ramSize, object + guest_.raceDynamicsOffset);
  return dynamics != 0 && dynamics != object && static_cast<std::size_t>(dynamics) + guest_.dynamicsBytes <= ramSize;
}

std::uint32_t Controller::flare_count(std::uint32_t offset, const std::uint8_t *ram, std::size_t ramSize) const {
  return read_word(ram, ramSize, raceObject_ + guest_.raceFlareOffset + offset);
}

// The next frame's update runs after this. The flare queues are filled in update and emptied in render, so a frame left out
// must not leave its entries for the next one (they would pile up over the queue's size).
void Controller::new_frame(std::uint8_t *ram, std::size_t ramSize) {
  if (clearFlares_ && raceObject_ != 0) {
    write_word(ram, ramSize, raceObject_ + guest_.raceFlareOffset + guest_.flareTrackCountOffset, 0);
    write_word(ram, ramSize, raceObject_ + guest_.raceFlareOffset + guest_.flareCarCountOffset, 0);
  }
  clearFlares_ = false;
  skippingFrame_ = false;
  raceSeenThisFrame_ = false;
}

void Controller::evaluate_gate(std::uint32_t clock) {
  const bool rose = hasLastClock_ && clock != 0 && clock == lastClock_ + kClockStep;
  lastClock_ = clock;
  hasLastClock_ = true;
  if (rose) {
    if (++goodFrames_ >= kGateFrames) gateOpen_ = true;
    return;
  }
  if (gateOpen_) ++stats_.gateCloses;
  gateOpen_ = false;
  goodFrames_ = 0;
}

bool Controller::should_skip() {
  if (forcePattern_ >= 1) return forceCounter_++ % static_cast<std::uint64_t>(forcePattern_) != 0;
  return adaptive_ && policy_.decide(slackUs_, gsLateUs_, std::max(gsCostUs_, eeCostUs_));
}

// Once per frame, at its first render functor, with the frame's update done.
void Controller::decide(std::uint8_t *ram, std::size_t ramSize) {
  std::uint32_t clock = 0;
  std::uint32_t dynamics = 0;
  if (raceSeenThisFrame_ && is_race(raceObject_, ram, ramSize)) {
    dynamics = read_word(ram, ramSize, raceObject_ + guest_.raceDynamicsOffset);
    clock = read_word(ram, ramSize, dynamics + guest_.dynamicsClockOffset);
  }
  evaluate_gate(clock);
  const std::uint32_t page = read_word(ram, ramSize, guest_.displayPage);
  const std::uint32_t pageSecond = read_word(ram, ramSize, guest_.displayPageSecond);
  // The words are 0 or 1, or this is not the state this was written for: leave the frame alone.
  const bool wordsFine = page <= 1 && pageSecond <= 1;
  skippingFrame_ = gateOpen_ && wordsFine && should_skip();
  if (!previousDrew_ && wordsFine) {
    // No list ran in this frame: nothing new to show. The flip inverts them back (see the header).
    write_word(ram, ramSize, guest_.displayPage, 1 - page);
    write_word(ram, ramSize, guest_.displayPageSecond, 1 - pageSecond);
  }
  previousDrew_ = !skippingFrame_;
  if (skippingFrame_) {
    clearFlares_ = true;
    ++stats_.skipped;
    stats_.longestRun = std::max(stats_.longestRun, ++run_);
    framesSinceSkip_ = 0;
    stats_.largestFlareQueue = std::max({stats_.largestFlareQueue, flare_count(guest_.flareTrackCountOffset, ram, ramSize),
                                         flare_count(guest_.flareCarCountOffset, ram, ramSize)});
  } else {
    ++stats_.rendered;
    run_ = 0;
    if (framesSinceSkip_ < 120) ++framesSinceSkip_;
  }
  static const char *dumpAt = std::getenv("GT4_FRAME_SKIP_DUMP_AT");
  if (dumpAt != nullptr && dynamics != 0 && vblank_ == static_cast<std::uint64_t>(std::atoll(dumpAt))) {
    // Diagnostics: the simulation state and the race object at one VBlank, to compare two runs by offset.
    if (std::FILE *file = std::fopen(std::getenv("GT4_FRAME_SKIP_DUMP_FILE"), "wb")) {
      std::fwrite(ram + dynamics, 1, guest_.dynamicsBytes, file);
      std::fwrite(ram + raceObject_, 1, 0x10000, file);
      std::fclose(file);
    }
  }
  if (log_ != nullptr) {
    const std::uint64_t digest = dynamics != 0 ? fnv1a(ram + dynamics, guest_.dynamicsBytes) : 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::fprintf(log_, "%llu,%d,%u,%d,%d,%d,%016llx,%lld,%d,%d\n", static_cast<unsigned long long>(vblank_), skippingFrame_ ? 0 : 1, clock,
                 slackUs_ == Policy::kNoSlack ? 0 : slackUs_, gsLateUs_, gateOpen_ ? 1 : 0, static_cast<unsigned long long>(digest),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()), gsCostUs_, eeCostUs_);
  }
}

bool Controller::on_functor(std::uint32_t pc, std::uint32_t object, std::uint8_t *ram, std::size_t ramSize) {
  if (pc == guest_.updateFunctor) {
    if (phase_ != Phase::update) new_frame(ram, ramSize);
    phase_ = Phase::update;
    if (is_race(object, ram, ramSize)) {
      raceObject_ = object;
      raceSeenThisFrame_ = true;
    }
    return false;  // the simulation always runs
  }
  if (pc == guest_.renderFunctor) {
    // A frame may have no update at all (the game can hold it), and then it starts here.
    if (phase_ == Phase::idle) new_frame(ram, ramSize);
    if (phase_ != Phase::render) {
      phase_ = Phase::render;
      decide(ram, ramSize);
    }
    return skippingFrame_;
  }
  phase_ = Phase::idle;
  return skippingFrame_;
}

bool g_enabled = false;

Controller &controller() {
  static Controller instance;
  return instance;
}

namespace {
std::FILE *g_log = nullptr;
}

void start_from_environment() {
  const char *forceText = std::getenv("GT4_FRAME_SKIP_FORCE");
  const char *onText = std::getenv("GT4_FRAME_SKIP");
  const int force = forceText != nullptr ? std::atoi(forceText) : 0;
  const bool adaptive = onText != nullptr && std::strcmp(onText, "1") == 0;
  if (force < 1 && !adaptive) return;  // 1 leaves nothing out: the reference run of the log
  // The checks hold for 480p, which the launcher always asks for. In the game's own 480i every picture weaves two fields, and that is
  // not tested: the display is frame by frame unlike the same drive with nothing left out.
  const char *progressive = std::getenv("GT4_FORCE_480P");
  if (progressive == nullptr || std::strcmp(progressive, "1") != 0) {
    std::fprintf(stderr, "[frame-skip] needs GT4_FORCE_480P=1: off\n");
    return;
  }
  const auto &hooks = ::gt4::target::active().hooks;
  if (hooks.frame_render_functor == 0) {
    std::fprintf(stderr, "[frame-skip] this target has no addresses for it: off\n");
    return;
  }
  if (const char *path = std::getenv("GT4_FRAME_SKIP_LOG")) {
    g_log = std::fopen(path, "w");
    if (g_log != nullptr) std::fputs("vblank,rendered,clock,slack_us,gs_late_us,gate,digest,wall_us,gs_cost_us,ee_cost_us\n", g_log);
  }
  Guest guest;
  guest.updateFunctor = hooks.frame_update_functor;
  guest.renderFunctor = hooks.frame_render_functor;
  guest.idleFunctor = hooks.frame_idle_functor;
  guest.displayPage = hooks.frame_display_page;
  guest.displayPageSecond = hooks.frame_display_page_second;
  guest.raceVtableFirst = hooks.frame_race_vtable_first;
  guest.raceVtableLast = hooks.frame_race_vtable_last;
  guest.raceDynamicsOffset = hooks.frame_race_dynamics_offset;
  guest.dynamicsClockOffset = hooks.frame_dynamics_clock_offset;
  guest.dynamicsBytes = hooks.frame_dynamics_bytes;
  guest.raceFlareOffset = hooks.frame_race_flare_offset;
  guest.flareTrackCountOffset = hooks.frame_flare_track_count_offset;
  guest.flareCarCountOffset = hooks.frame_flare_car_count_offset;
  controller().configure(guest, force, adaptive, g_log);
  g_enabled = true;
  std::atexit(finish);
  std::fprintf(stderr, "[frame-skip] on: %s\n", force >= 1 ? "fixed pattern" : "when behind");
}

void print_report(std::FILE *stream) {
  if (!g_enabled) return;
  const Stats &s = controller().stats();
  std::fprintf(stream, "[frame-skip] rendered=%llu skipped=%llu longest run=%llu gate closes=%llu largest flare queue=%u\n",
               static_cast<unsigned long long>(s.rendered), static_cast<unsigned long long>(s.skipped),
               static_cast<unsigned long long>(s.longestRun), static_cast<unsigned long long>(s.gateCloses),
               s.largestFlareQueue);
}

void finish() {
  if (g_log != nullptr) std::fflush(g_log);
  print_report(stderr);
}

}  // namespace gt4::frame_skip
