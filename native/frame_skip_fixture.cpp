// CTest native_frame_skip: TASK-082, the frame skip controller on a guest memory of its own (frame_skip.hpp), host only.
#include "frame_skip.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>

using gt4::frame_skip::Controller;
using gt4::frame_skip::Guest;
using gt4::frame_skip::Policy;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "native_frame_skip: %s\n", what);
  std::exit(1);
}

constexpr std::uint32_t kUpdate = 0x1000, kRender = 0x1100, kIdle = 0x1200;
constexpr std::uint32_t kPage = 0x2000, kPageSecond = 0x2004;
constexpr std::uint32_t kRace = 0x100000, kDynamics = 0x200000, kOtherObject = 0x300000;
constexpr std::uint32_t kRaceVtable = 0x5000, kOtherVtable = 0x9000;
constexpr std::uint32_t kClockOffset = 0xF898, kFlare = 0xDA0, kFlareTrack = 0x1848, kFlareCar = 0xA34;

struct Game {
  std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(0x400000);
  Controller control;
  std::uint32_t displayed = 0;  // the frame number the shown page holds
  std::uint32_t pages[2] = {0, 0};
  std::uint32_t frame = 0;
  std::uint32_t list = 0;        // the frame the drawing list built last holds, 0 for an empty list
  std::uint32_t listTarget = 0;  // the page it draws into
  int drawnIntoShownPage = 0;
  std::uint32_t clock = 1000;
  bool running = true;
  int rendersRun = 0, idlesRun = 0;

  std::uint32_t read(std::uint32_t address) const {
    std::uint32_t value = 0;
    std::memcpy(&value, ram.data() + address, 4);
    return value;
  }
  void write(std::uint32_t address, std::uint32_t value) { std::memcpy(ram.data() + address, &value, 4); }

  explicit Game(int force, bool adaptive = false) {
    Guest guest;
    guest.updateFunctor = kUpdate;
    guest.renderFunctor = kRender;
    guest.idleFunctor = kIdle;
    guest.displayPage = kPage;
    guest.displayPageSecond = kPageSecond;
    guest.raceVtableFirst = kRaceVtable;
    guest.raceVtableLast = kRaceVtable + 0x100;
    guest.raceDynamicsOffset = 0x70;
    guest.dynamicsClockOffset = kClockOffset;
    guest.dynamicsBytes = 0x10000;
    guest.raceFlareOffset = kFlare;
    guest.flareTrackCountOffset = kFlareTrack;
    guest.flareCarCountOffset = kFlareCar;
    control.configure(guest, force, adaptive, nullptr);
    write(kRace + 0x64, kRaceVtable);
    write(kRace + 0x70, kDynamics);
    write(kOtherObject + 0x64, kOtherVtable);
  }

  // One main loop iteration, as the game runs it: update, render, idle of each object, then the flip.
  void run_frame() {
    ++frame;
    // The list of the last frame runs now, into its page, with the shown page as it was at the last flip.
    if (list != 0) {
      if (listTarget == read(kPage)) ++drawnIntoShownPage;
      pages[listTarget] = list;
    }
    list = 0;
    control.on_functor(kUpdate, kOtherObject, ram.data(), ram.size());
    control.on_functor(kUpdate, kRace, ram.data(), ram.size());
    if (running) clock += Controller::kClockStep;
    write(kDynamics + kClockOffset, clock);
    write(kRace + kFlare + kFlareTrack, read(kRace + kFlare + kFlareTrack) + 3);  // update fills the flare queues
    write(kRace + kFlare + kFlareCar, read(kRace + kFlare + kFlareCar) + 1);
    for (std::uint32_t object : {kOtherObject, kRace}) {
      if (!control.on_functor(kRender, object, ram.data(), ram.size())) {
        ++rendersRun;
        list = frame;
        listTarget = read(kPage);  // the page the flip at the end of this frame leaves in the back
        if (object == kRace) {
          write(kRace + kFlare + kFlareTrack, 0);  // render empties them
          write(kRace + kFlare + kFlareCar, 0);
        }
      }
    }
    for (std::uint32_t object : {kOtherObject, kRace}) {
      if (!control.on_functor(kIdle, object, ram.data(), ram.size())) ++idlesRun;
    }
    write(kPage, 1 - read(kPage));  // the flip, unconditional
    write(kPageSecond, 1 - read(kPageSecond));
    displayed = pages[read(kPage)];
  }
};

}  // namespace

int main() {
  {  // Fixed pattern: the display never goes back to an older frame, and shows the last frame drawn.
    Game game(2);
    game.write(kPageSecond, 1);
    std::uint32_t last = 0;
    std::uint32_t skippedFrames = 0;
    for (int i = 0; i < 400; ++i) {
      game.run_frame();
      check(game.displayed >= last, "the display went back to an older frame");
      if (game.displayed == last) ++skippedFrames;
      // A list runs a frame after it was built, so the last frame shown is at least one old, and a frame left out adds one.
      check(i < 2 || game.frame - game.displayed <= 2, "the display is more than the pattern's frames behind");
      last = game.displayed;
    }
    check(game.drawnIntoShownPage == 0, "a drawing list ran into the page on display");
    check(skippedFrames > 150, "the pattern did not skip one frame in two");
    check(game.control.stats().skipped > 150 && game.control.stats().longestRun == 1, "the pattern's counts are wrong");
    check(game.control.stats().largestFlareQueue <= 4, "the flare queues piled up over frames left out");
  }
  {  // One render in three.
    Game game(3);
    std::uint32_t last = 0;
    for (int i = 0; i < 400; ++i) {
      game.run_frame();
      check(game.displayed >= last, "the display went back with one frame in three");
      check(i < 3 || game.frame - game.displayed <= 3, "more than two frames in a row left out");
      last = game.displayed;
    }
    check(game.drawnIntoShownPage == 0, "a drawing list ran into the page on display, one frame in three");
    check(game.control.stats().longestRun == 2, "one frame in three must leave out two in a row");
  }
  {  // The first attempt's defect, as a control of this fixture: without the words inverted the display goes back.
    Game game(0);
    std::uint32_t wentBack = 0, last = 0, list = 0, target = 0;
    for (int i = 0; i < 100; ++i) {
      ++game.frame;
      if (list != 0) game.pages[target] = list;
      list = game.frame % 2 == 1 ? game.frame : 0;  // draws one frame in two, flips every frame
      target = game.read(kPage);
      game.write(kPage, 1 - game.read(kPage));
      game.displayed = game.pages[game.read(kPage)];
      if (game.displayed < last) ++wentBack;
      last = game.displayed;
    }
    check(wentBack > 20, "the model of the flip does not show the first attempt's defect");
  }
  {  // Any mix of frames drawn and left out: never back, never into the shown page. The policy's pattern, driven by slack.
    Game game(0, true);
    std::uint32_t last = 0;
    std::uint32_t seed = 12345;
    for (int i = 0; i < 2000; ++i) {
      seed = seed * 1103515245u + 12345u;
      game.control.note_vblank(static_cast<std::uint64_t>(i), (seed >> 16) % 3 == 0 ? -60000 : 1000, -1000, 40000, 5000);
      game.run_frame();
      check(game.displayed >= last, "the display went back in a mix of frames");
      check(i < 5 || game.frame - game.displayed <= static_cast<std::uint32_t>(Policy::kMaxRun) + 2,
            "the display fell behind in a mix of frames");
      last = game.displayed;
    }
    check(game.drawnIntoShownPage == 0, "a drawing list ran into the page on display in a mix of frames");
    check(game.control.stats().skipped > 200 && game.control.stats().longestRun >= 2 &&
              game.control.stats().longestRun <= static_cast<std::uint64_t>(Policy::kMaxRun),
          "the mix did not leave frames out, or left out more in a row than allowed");
  }
  {  // No skipping before the race clock has risen for a second; off in menus and when paused.
    Game game(2);
    for (std::uint32_t i = 0; i < Controller::kGateFrames; ++i) {
      game.run_frame();
      check(game.control.stats().skipped == 0, "skipped before the gate opened");
    }
    for (int i = 0; i < 10; ++i) game.run_frame();
    check(game.control.gate_open() && game.control.stats().skipped > 0, "the gate did not open");
    game.running = false;
    game.run_frame();
    check(!game.control.gate_open(), "the gate stayed open on a paused race");
    const std::uint64_t skipped = game.control.stats().skipped;
    for (int i = 0; i < 100; ++i) game.run_frame();
    check(game.control.stats().skipped == skipped, "skipped on a paused race");
    check(game.displayed + 1 == game.frame, "a paused race did not show every frame");
  }
  {  // TASK-146: the intro before the start (race object there, clock still 0) skips as a running race, then the race does.
    Game game(2);
    game.clock = 0;
    game.running = false;
    for (std::uint32_t i = 0; i < Controller::kGateFrames; ++i) game.run_frame();
    for (int i = 0; i < 20; ++i) game.run_frame();
    check(game.control.gate_open() && game.control.stats().skipped > 0, "the intro with the clock at 0 did not open the gate");
    game.running = true;  // the green flag: 0 -> 50 -> 100 ...
    const std::uint64_t closes = game.control.stats().gateCloses;
    for (int i = 0; i < 20; ++i) game.run_frame();
    check(game.control.gate_open() && game.control.stats().gateCloses == closes, "the start of the race closed the gate");
    check(game.drawnIntoShownPage == 0, "a drawing list ran into the page on display across the start");
  }
  {  // No race object: a menu never skips.
    Game game(2);
    game.write(kRace + 0x64, kOtherVtable);
    for (int i = 0; i < 300; ++i) game.run_frame();
    check(game.control.stats().skipped == 0, "skipped without a race");
  }
  {  // A frame with no update starts at its render.
    Game game(2);
    for (int i = 0; i < 100; ++i) game.run_frame();
    const std::uint64_t decided = game.control.stats().rendered + game.control.stats().skipped;
    game.control.on_functor(kRender, kRace, game.ram.data(), game.ram.size());
    game.control.on_functor(kIdle, kRace, game.ram.data(), game.ram.size());
    check(game.control.stats().rendered + game.control.stats().skipped == decided + 1, "a frame with no update was not decided");
    check(!game.control.gate_open(), "a frame with no update must close the gate: the race object was not seen");
  }
  {  // Words that are not 0 or 1 are not the state this was written for.
    Game game(2);
    for (int i = 0; i < 100; ++i) game.run_frame();
    game.write(kPage, 7);
    const std::uint64_t skipped = game.control.stats().skipped;
    game.control.on_functor(kUpdate, kRace, game.ram.data(), game.ram.size());
    game.clock += Controller::kClockStep;
    game.write(kDynamics + kClockOffset, game.clock);
    check(!game.control.on_functor(kRender, kRace, game.ram.data(), game.ram.size()), "skipped with a page word out of range");
    check(game.control.stats().skipped == skipped && game.read(kPage) == 7, "a page word out of range was changed");
  }
  {  // The policy: nothing on time, or a little late.
    Policy policy;
    for (int i = 0; i < 100; ++i) check(!policy.decide(2000, -3000, 14000), "skipped on time");
    check(!policy.decide(-Policy::kLateUs + 1000, 0, 25000), "skipped with a small delay");
    check(!policy.decide(1000, Policy::kLateUs - 1000, 25000), "skipped with a small delay of the GS worker");
    check(!policy.decide(Policy::kNoSlack, Policy::kLateUs * 2, 25000), "skipped without a pacer");
    check(!policy.in_shortfall(), "a shortfall with no lateness");
  }
  {  // A lasting shortfall: frames left out evenly, as many as the cost asks for.
    for (const auto &[cost, drawnLow, drawnHigh] : {std::tuple{21700, 44, 47}, std::tuple{33000, 28, 31}, std::tuple{50000, 19, 21}}) {
      Policy policy;
      check(policy.decide(-Policy::kLateUs - 1000, 0, cost) || policy.in_shortfall(), "no shortfall when late");
      int drawn = 0, run = 0, longest = 0;
      for (int i = 0; i < 600; ++i) {
        const bool skip = policy.decide(5000, -1000, cost);
        drawn += skip ? 0 : 1;
        run = skip ? run + 1 : 0;
        longest = run > longest ? run : longest;
      }
      check(drawn / 10 >= drawnLow && drawn / 10 <= drawnHigh, "the frames drawn do not follow the cost");
      check(longest <= Policy::kMaxRun, "more in a row than allowed");
      if (cost == 21700) check(longest == 1, "frames left out are not spread evenly");
    }
  }
  {  // The floor: more than the game can pay for, and the game slows down at one frame drawn in kMaxRun + 1.
    Policy policy;
    policy.decide(-Policy::kLateUs - 1000, 0, 90000);
    int run = 0;
    for (int i = 0; i < 300; ++i) {
      run = policy.decide(-30000, 30000, 90000) ? run + 1 : 0;
      check(run <= Policy::kMaxRun, "under the floor of frames drawn");
    }
  }
  {  // Being behind costs more frames, and the shortfall ends when on time and within budget for two seconds.
    Policy policy;
    policy.decide(-Policy::kLateUs - 1000, 0, 16000);
    int left = 0;
    for (int i = 0; i < 60; ++i) left += policy.decide(-30000, 10000, 16000) ? 1 : 0;
    check(left > 0, "no frame left out to get back on time");
    for (int i = 0; i < Policy::kCalmFrames - 1; ++i) policy.decide(5000, -5000, 15000);
    check(policy.in_shortfall(), "the shortfall ended too soon");
    for (int i = 0; i < 2; ++i) policy.decide(5000, -5000, 15000);
    check(!policy.in_shortfall(), "the shortfall did not end");
    check(!policy.decide(5000, -5000, 15000), "skipped after the shortfall");
  }
  std::puts("native_frame_skip: ok (patterns, the first attempt's defect, gate, menus, no update, page words, policy, shortfall)");
  return 0;
}
