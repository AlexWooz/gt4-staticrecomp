#pragma once
// TASK-082: frame skipping. The simulation stays at 60 Hz, and only the drawing of a frame is left out when the host falls behind
// (decisions/native-variable-fps-plan: N1, P1).
//
// The game's main loop (v2.00 func_001016E8) runs per frame: update of every object, render of every object, the DMA-safe idle
// of every object, then the flip, which waits for the VBlank. The three functors are called through pointers, so they pass through
// the runtime's dispatch, and leaving out a frame's drawing is returning from the render and idle functors at once.
//
// The flip is the trap (the stale image of the first attempt, docs/TASK-082-RESUMPTION-REPORT.md). The game builds a frame's drawing
// list during that frame, and the list runs during the next frame, into the page not shown; the flip at the end of that next frame
// shows it. The flip always inverts the page index and points DISPFB at the other page, so a frame whose list was empty shows
// the page from before the last one. And a list is built for the page that the flip of its own frame leaves in the back.
//
// So the rule: a frame whose previous frame drew nothing has nothing to show, and must not flip. The flip is a direct call, out of
// reach here, so such a frame inverts both page words before drawing: the list it builds goes to the page in the back, the flip
// inverts them back, and DISPFB gets the page it already had. Any other frame flips as always, drawn or not, and shows the list that
// just ran.
//
// Off by default: GT4_FRAME_SKIP=1 skips when behind, GT4_FRAME_SKIP_FORCE=2|3 keeps 1 frame in 2 or 3 (tests). Only in a running
// race: the race clock must rise by one step per frame for a second; pause, menus and accelerated time close that gate at once.
// GT4_FRAME_SKIP_LOG=<csv>: one row per frame, for scripts/coregs2_bench/frame_sheet.py.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "game_target.hpp"

namespace gt4::frame_skip {

// Whether to leave out the next frame's drawing, from how far behind the host is. Pure, so the fixture can drive it.
class Policy {
public:
  static constexpr std::int32_t kLateUs = 12000;  // behind the pacer's grid by this much after the last frame
  static constexpr std::size_t kQueuedFrames = 2;  // the GS worker this many VBlanks behind: the GPU is what is late
  static constexpr int kMaxRun = 2;  // at least 20 frames per second; below that the game slows down instead
  static constexpr std::int32_t kNoSlack = INT32_MIN;

  // `slackUs`: time left before the VBlank was due (negative: late), kNoSlack without a pacer.
  bool decide(std::int32_t slackUs, std::size_t gsVsyncsQueued) {
    const bool late = slackUs != kNoSlack && slackUs < -kLateUs;
    const bool skip = (late || gsVsyncsQueued >= kQueuedFrames) && run_ < kMaxRun;
    run_ = skip ? run_ + 1 : 0;
    return skip;
  }

private:
  int run_ = 0;
};

struct Stats {
  std::uint64_t rendered = 0;
  std::uint64_t skipped = 0;
  std::uint64_t longestRun = 0;
  std::uint64_t gateCloses = 0;
  std::uint32_t largestFlareQueue = 0;
};

// The guest addresses this needs, from game_target.hpp.
struct Guest {
  std::uint32_t updateFunctor = 0;
  std::uint32_t renderFunctor = 0;
  std::uint32_t idleFunctor = 0;
  std::uint32_t displayPage = 0;
  std::uint32_t displayPageSecond = 0;
  std::uint32_t raceVtableFirst = 0;
  std::uint32_t raceVtableLast = 0;
  std::uint32_t raceDynamicsOffset = 0;
  std::uint32_t dynamicsClockOffset = 0;
  std::uint32_t dynamicsBytes = 0;  // the simulation state the log's digest covers
  std::uint32_t raceFlareOffset = 0;
  std::uint32_t flareTrackCountOffset = 0;
  std::uint32_t flareCarCountOffset = 0;
};

class Controller {
public:
  static constexpr std::uint32_t kClockStep = 50;  // DynamicsConductor race clock per simulation step
  static constexpr std::uint32_t kGateFrames = 60;

  void configure(const Guest &guest, int forcePattern, bool adaptive, std::FILE *log);

  // On the EE, from dispatch: true means this functor returns at once.
  bool on_functor(std::uint32_t pc, std::uint32_t object, std::uint8_t *ram, std::size_t ramSize);

  // Inputs of the policy, set by the host at each VBlank.
  void note_vblank(std::uint64_t vblank, std::int32_t slackUs, std::size_t gsVsyncsQueued) {
    vblank_ = vblank;
    slackUs_ = slackUs;
    gsVsyncsQueued_ = gsVsyncsQueued;
  }

  bool gate_open() const { return gateOpen_; }
  // A frame was left out in the last 2 seconds of frames.
  bool skipping_recently() const { return framesSinceSkip_ < 120; }
  const Stats &stats() const { return stats_; }

private:
  enum class Phase { update, render, idle };

  bool is_race(std::uint32_t object, const std::uint8_t *ram, std::size_t ramSize) const;
  void new_frame(std::uint8_t *ram, std::size_t ramSize);
  void decide(std::uint8_t *ram, std::size_t ramSize);
  void evaluate_gate(std::uint32_t clock);
  bool should_skip();
  std::uint32_t flare_count(std::uint32_t offset, const std::uint8_t *ram, std::size_t ramSize) const;

  Guest guest_{};
  int forcePattern_ = 0;
  bool adaptive_ = false;
  std::FILE *log_ = nullptr;
  Policy policy_;

  Phase phase_ = Phase::idle;
  bool skippingFrame_ = false;
  bool previousDrew_ = true;
  bool clearFlares_ = false;
  std::uint32_t raceObject_ = 0;
  bool raceSeenThisFrame_ = false;

  bool gateOpen_ = false;
  std::uint32_t goodFrames_ = 0;
  std::uint32_t lastClock_ = 0;
  bool hasLastClock_ = false;
  std::uint64_t forceCounter_ = 0;
  std::uint64_t run_ = 0;
  std::uint64_t framesSinceSkip_ = 120;

  std::uint64_t vblank_ = 0;
  std::int32_t slackUs_ = Policy::kNoSlack;
  std::size_t gsVsyncsQueued_ = 0;
  Stats stats_{};
};

// The one instance of the game; the runtime's dispatch tests g_enabled first, so a game with this off pays one branch.
extern bool g_enabled;
Controller &controller();

// From the environment. Prints what it enabled, and refuses a target without its addresses.
void start_from_environment();
void print_report(std::FILE *stream);
void finish();

inline bool on_dispatch(std::uint32_t pc, std::uint32_t object, std::uint8_t *ram, std::size_t ramSize) {
  const auto &hooks = ::gt4::target::active().hooks;
  if (pc != hooks.frame_update_functor && pc != hooks.frame_render_functor && pc != hooks.frame_idle_functor) return false;
  return controller().on_functor(pc, object, ram, ramSize);
}

}  // namespace gt4::frame_skip
